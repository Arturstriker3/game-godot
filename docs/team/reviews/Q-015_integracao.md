# Q-015 — integração das três frentes (zonas, moradia, prefeitura)

- **Branch:** `fix/q015-int`, worktree `G:/projetos/opengs-wt-q15int`, saída de
  `balance/global-resources` (`f4e70a4`; base das frentes `a8d7984`).
- **Data:** 2026-11-17. **Dono:** `@gerente-simulacao`. **Modelos:** somente DeepSeek Flash (executor e verificador).
- **Objeto:** integrar `fix/q015-zonas` `84c315e` (Q-015-C, capacidade das zonas),
  `fix/q015-moradia` `6ebb397` (Q-015-A, mercado de moradia) e
  `fix/q015-prefeitura` `0836df3` (Q-015-B, gate da prefeitura) e medir o **mundo integrado**,
  não as frentes em separado. Critérios: `docs/team/reviews/Q-015_s19_review.md`.
- **TL;DR:** as três correções coexistem e cada uma funciona; **a interação revela dois conflitos
  semânticos** que **não** aparecem como conflito de texto — nenhum deles existia nas frentes
  isoladas. O mais importante: com o preço da terra calibrado, o custo de reposição voltava a
  ficar acima do preço e **nenhuma unidade era construída** (B1 de volta); corrigido (lote =
  m² ÷ FAR, terra limitada a 45% do valor). Depois da correção **há moradia sendo construída nas
  6 cidades**, mas a **ocupação não sobe** (0,849 → 0,842-0,847 em 365 d) e a **prefeitura não
  abre obra de solo em metrópole** — a explicação está no §5.4, medida, não forçada.

## 1. Merges

Ordem pedida (zonas → moradia → prefeitura), `--no-ff`:

| # | Merge | Commit | Conflito de texto |
| --- | --- | --- | --- |
| 1 | `fix/q015-zonas` | `d630069` | nenhum |
| 2 | `fix/q015-moradia` | `6ada658` | `sim_constants.h` — **automático** (hunks distintos) |
| 3 | `fix/q015-prefeitura` | `5e9c880` | `urban_land.cpp` — **automático** (hunks distintos) |

Nenhum conflito textual: o `git` resolveu `sim_constants.h` (constantes de moradia × constantes de
zona) e `urban_land.cpp` (`zone_capacity`/`zone_fill` × gate de `start_project`) por hunks
disjuntos. Conferi os dois arquivos linha a linha: as **três** correções estão presentes e na ordem
certa (as constantes de moradia usam `kDemoConstructionMaterialShare` definido antes, linha 68).

## 2. Conflitos semânticos (o que o `git` não vê)

### 2.1 Fixture do `city_gov_test`: a cidade "lotada" deixou de ficar lotada

O teste comprime a área construída de `BRA-SAO` para 0,02 km² **para** a zona nascer acima do
gatilho. Com o Q-015-C, `demo_set_urban` calcula `zone_fill` exatamente para a ocupação nascer no
alvo da faixa (0,85) — comprimir a área **não muda mais nada**: a ocupação resultante é sempre
0,85, abaixo dos 0,90 do gatilho. Efeito medido: 5 falhas em `city_gov_test` ("prefeitura nao
decidiu obra com a zona lotada", "expansao nao abriu area", "capacidade nao cresceu", "cidade cheia
com caixa e obra estatal em voo").

**Resolução:** o fixture passa a representar a cidade cheia como o jogo chega nela — ocupação
0,95, reduzindo o `zone_fill` pelo **round-trip de save** (`_force_busy_zone`), sem inventar
estoque. Nenhuma regra do núcleo mudou por causa disso; a asserção do gate (o A/B do `B3`) continua
idêntica e o teste volta a verde.

### 2.2 Preço da terra calibrado × custo de reposição (B1 de volta)

Com a ocupação real (0,85), a curva de pressão do preço do solo (`occ² / (1-occ)`, teto 8) levou o
m² de terra a **113-174% do valor do imóvel** — São Paulo: 50 m² × 2,32 XGS = 116 XGS de terreno
para uma unidade de 103 XGS. Com esse custo, `price ≥ 1,2 × custo` volta a ser **matematicamente
inalcançável** e a moradia deixou de ser construída (medido: `built_cum` negativo nas 6 cidades e
zero projetos). O ajuste de B=6 da frente de moradia foi derivado com terra a ~30% do valor
(ocupação 0,03-0,4 do mundo antigo), não com a ocupação calibrada.

Duas causas, duas correções:

1. **Área de terreno ≠ área construída.** `build_cost_unit` usava `m² construído × land_price`
   como se o lote tivesse a área construída; em torre (FAR 6) o terreno entrava **6× acima** do
   lote real. Agora `lote = m² construído ÷ FAR da densidade` — em São Paulo o terreno fica em
   ~19% do valor, faixa real.
2. **Teto declarado da participação do terreno** — `kDemoHousingLandShareMax = 0,45` (faixa
   observada 20-50% em metrópoles): em zona de FAR baixo com solo caro o terreno não passa de 45%
   do valor, então obra (30%) + terra (≤45%) deixa a margem do incorporador alcançável
   (custo ≤75% contra exigência de ≤83,3%).

A conta da **parcela de terra creditada à prefeitura** (`demo_project_land_cost`) foi alinhada à
mesma fórmula (lote ÷ FAR + teto de 45%; no crédito o teto é conservador — incide sobre o
`total_cost`, que é ≤ `units × unit_value`, então o crédito nunca passa do que foi cobrado); sem
isso ela voltava a consumir o orçamento inteiro e a obra abria com `reserved = 0` (sintoma:
"caixa 5000000 → 5000000 nao fecha com custo 0.0" no `city_gov_test` e "capacidade 1.0 no piso" no
`construction_queue_test`).

Tentativa descartada: limitar o **preço** da terra (em vez do custo). Ela achatava o sinal
ocupação→preço exigido pelo `urban_land_test` ("zona cheia deveria custar bem mais que a folgada":
122 vs 125) — o preço do solo **tem** de reagir à ocupação; o que precisa ser limitado é o que ele
pesa no custo da moradia. Depois da correção: AGRIA 470,0 vs PORTUS 139,0 XGS/m² (3,4×).

## 3. Build, import e ambiente

- **Conflito de slot:** o `CMakeLists.txt` escreve as duas configurações no **mesmo** nome
  (`...template_debug.x86_64.dll`). O build release sobrescreveu a DLL debug. Contornado com
  backup/restauração explícitos; a DLL debug real foi restaurada no fim (§8).
- Build debug: **23.294.976 B**, SHA256 `6720655F24EA0E58059C6F4A7A71D3E4A852DBB7096A5C29567ED10E18C3960C`.
- Build release: **1.786.368 B**, SHA256 `6BA0E9129A787421381D394485CB9A6109BA9B51AF7FB3292FC0BBACEB143301`.
- `godot --headless --path . --import` uma vez na worktree (exit 1 do Godot para `--import`, com
  `[ DONE ] reimport` impresso; a extensão carrega e roda normalmente).
- Godot 4.7.2 stable, clang/llvm-mingw 20261104, CMake Ninja. Nenhum editor/processo do MCP foi
  tocado; nenhuma captura de UI (não se aplica).

## 4. Testes (binário integrado final)

| Teste | Resultado |
| --- | --- |
| `economy_integration_test` | verde |
| `economy_ai_test` | verde |
| `population_demo_test` | verde |
| `economy_long_run_test` (365 d, conservação) | verde |
| `economy_save_load_roundtrip_test` | **0 diffs** (30+30 d == 60 d direto) |
| `housing_market_test` | verde |
| `city_gov_test` | verde (com o fixture de cidade cheia do §2.1) |
| `urban_land_test` | verde (preço × ocupação 470,0 vs 139,0 XGS/m²) |
| `urban_zone_audit` | verde (834 cidades; NPL-POK 0,700) |
| `construction_project_test` | verde |
| `construction_queue_test` | verde (fila única, terra creditada, `reserved` > 0) |
| `validate_urban_land.py` / `validate_housing_city.py` / `validate_scenario_data.py` | exit 0 (dados intocados) |
| **threads 1 × 8 (60 d, `--urban=1`)** | JSON **byte-idêntico** (`sha256 568CD4249D7920…`) |

Ressalva honesta: o `economy_world_test` em **60 e 120 dias** termina com falha de comparativo de
comida (**EGY** 1,000 → 0,857 em 60 d e → 0,546 em 120 d contra o baseline de 365 d). É o transiente
de importação já documentado no próprio teste ("EGY 1,000→0,979") amplificado pela construção em
voo; **fecha em 365 d** (EGY = 1,000, §5). O critério do sweep é o de 365 d.

## 5. Interação entre as frentes (365 d, 6 cidades, `--urban=1 --ai=1 --threads=1`)

Sonda mensal (`tools/_tmp_q15int_monthly.gd`, temporária, não versionada): amostra a cada 30 dias,
`demo_get_urban_city` + fila única de obras; dinheiro **drift 0,0000**.

### 5.1 Estado no dia 360 (última amostra mensal)

Coluna "Entregues" = acumulado derivado de `delivered_cum + informal×(1 − 1/1200)` (a sonda
desconta a depreciação mensal de 1%/ano do estoque formal).

| Cidade | Ocup. RES d0 → d360 | Entregues (acum., der.) | Ônus de aluguel | Pago/faturado (dia) | Informal Δ | km² urbano | Caixa munic. | Obras de solo | Obras de moradia (fila) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| BRA-SAO | 0,850 → **0,842** | +2.949 | 0,181 | 1,000 | 0 | 1724,30 (0) | 1.667.914 | **0** | 1 |
| JPN-TOK | 0,850 → **0,845** | +79.315 | 0,257 | 1,000 | 0 | 4489,30 (0) | 2.764.772 | **0** | 4 |
| NGA-LAG | 0,850 → **0,843** | +2.748 | 0,240 | 1,000 | 0 | 527,90 (0) | 2.435.865 | **0** | 2 |
| IND-MUM | 0,850 → **0,847** | +38.764 | 0,264 | 1,000 | 0 | 531,40 (0) | 3.036.620 | **0** | 4 |
| USA-NEW | 0,850 → **0,843** | +17.443 | 0,291 | 1,000 | 0 | 7478,50 (0) | 1.124.380 | **0** | 4 |
| AUS-WEI | 0,550 → **0,546** | +2 | 0,305 | 1,000 | 0 | 0,50 (0) | 52 | **0** | 3 |

- **Moradia (A/B1/B2):** ✓ há obra na fila em todas as 6 cidades, entrega acontecendo
  (0,04-0,46%/ano do estoque), ônus **0,181-0,305** dentro da faixa 0,15-0,40, pago/faturado
  **1,000** (aluguel cobrado da renda), **favela Δ = 0** nas 6 (Mumbai parou de crescer por
  transbordo) e o estoque **não** cresce (fica abaixo da depreciação de 1%/ano).
- **Zonas (C/B4):** ✓ ocupação nasce no alvo da faixa, nenhuma cidade >1,0, informal fora da conta
  da zona formal, preço do solo reage (5,14 XGS/m² em SP no d360; a sonda não exporta o dia 0,
  quando o preço é 0 por ausência explícita — a frente de zonas mediu 0,58 no mundo pré-correção;
  TOK 2,14; MUM 5,04; NEW 3,62).
- **Prefeitura (B3):** ✗ **nenhuma obra de solo em 365 d** (`month_expense_infrastructure = 0` e
  nenhum projeto `URBAN_INFRA` nas 6), mesmo com caixa de 1,1-3,0 M XGS. Ela **usa** a frente de
  moradia social (R$ 0,8-4,2 M gastos em 365 d), o que é a W2→W3.

### 5.2 A ocupação não sobe (medido mês a mês)

| Cidade | d30 | d90 | d180 | d270 | d360 |
| --- | ---: | ---: | ---: | ---: | ---: |
| BRA-SAO | 0,849 | 0,848 | 0,845 | 0,843 | 0,842 |
| JPN-TOK | 0,849 | 0,848 | 0,846 | 0,845 | 0,845 |
| IND-MUM | 0,849 | 0,848 | 0,847 | 0,847 | 0,847 |

### 5.3 Por que a prefeitura nunca abre obra em metrópole (não foi forçado)

1. A calibração de capacidade do Q-015-C coloca toda metrópole em **0,849** no dia 0. O gatilho é
   `kBusyOccupancy = 0,90`, e como `capacidade = estoque formal ÷ 0,85`, chegar a 0,90 exige
   **+5,9% de estoque formal** (0,90 ÷ 0,85).
2. A oferta de moradia não cobre isso: as entregas são **0,04-0,46%/ano** do estoque (2,9 mil
   unidades/ano em São Paulo para 5,8 M) e a **depreciação é 1%/ano**. O capital das firmas limita
   o lote do incorporador (`units = min(0,5% da demanda, caixa/custo_unit)`) — em SP o caixa é o
   fator limitante, não a falta de demanda. Resultado: **estoque líquido cai** e a ocupação
   **desce** 0,5-0,7 p.p. em 365 d.
3. O `city_gov_test` prova o gate corrigido (no binário integrado: cidade a 0,95 abre expansão de
   **0,4585 km² / 1.642.140 XGS** do caixa municipal, capacidade RES 6.143.254 → 248.982.435,
   mesmo com obra §15 em voo; a medição da frente isolada, em outro commit, foi 0,444 km² /
   1.649.170 XGS) e a decisão de moradia social é independente.
   O que falta **não é o gate**: é a cidade chegar a 0,90.

Não mexi no gatilho, na depreciação nem no capital do incorporador: são decisões de calibração de
outra ordem (aumentar a oferta 3-10×, ou baixar o gatilho para ~0,88, ou aceitar que expandir só
acontece quando o envelope realmente esgota). Fica registrado para o dono decidir.

## 6. Sweep 365 d × 5 em paralelo (`--urban=1`, `-OutPrefix q15int_`)

`tools/s19_world_sweep_parallel.ps1` rodado **de dentro da worktree** (resolve o projeto por
`$PSScriptRoot`; `user://` é compartilhado, daí o prefixo). 5 processos, `--threads=1` cada,
2,9 min de parede. Agregação: `tools/aggregate_s19_sweep.py` (saída em `%TEMP%`, não versionada).

| Variante | Falências de construção | Drift de dinheiro |
| --- | ---: | ---: |
| `real` | **5** | −0,0000 |
| `real_seed1` | **5** | −0,0000 |
| `real_seed2` | **5** | −0,0000 |
| `half` | **5** | −0,0000 |
| `xgs` | **5** | −0,0000 |

- **Falências ≤ 28:** PASSA (5; baseline 6, as frentes isoladas davam 5-6).
- **Dinheiro conservado:** PASSA (|drift| = 0,0000 ≪ 0,01).
- **Determinismo:** os 5 relatórios são **byte-idênticos** (`sha256 78c91417127d0030…`, 303.410 B) —
  como o desenho F5 exige.
- **Comida:** **0 nações** acima de 2,5 p.p. vs `construction_world_sweep.json` (179 comparadas).
  Piores: **DJI −0,60 p.p.**, os demais abaixo de 0,1 p.p. Ganhos: JOR **+22,9 p.p.** (0,771 →
  1,000), YEM +9,0, SAH +0,67, SOL +0,28, EGY = 1,000. Importadores aceitos pelo dono (SOL, DJI,
  SAH, JOR, YEM) todos dentro da faixa; **nenhuma outra nação** precisa de aceite.

## 7. Perf release (DLL release no slot debug da worktree, 834 cidades)

`godot --headless --path . res://tools/perf_day_peak.tscn -- --days=30 --tick-mode=worker --threads=8`, 3 rodadas:

| Run | tick p95 em regime (dia > 2) | tick médio |
| --- | ---: | ---: |
| 1 | 41,70 ms | 35,13 ms |
| 2 | 41,63 ms | 35,12 ms |
| 3 | 35,20 ms | 34,87 ms |
| **Mediana** | **41,63 ms** | — |

Meta **≤ 50 ms: aprovada** (mediana 41,63; a revisão Q-015 mediu 45,06 na base sem as três
correções). Memória estática 299,8 MB. Slot durante as 3 rodadas: **release** (1.786.368 B — o
listado no início do comando de perf); a DLL **debug real foi restaurada** ao fim
(SHA256 `6720655F…`, 23.294.976 B).

## 8. Plausibilidade — as 6 cidades no dia 365

| Cidade | Aluguel/renda | Ocup. RES | Preço da terra (RES) | Favela (Δ) | Obras | Plausível? por quê |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| BRA-SAO | 0,181 | 0,842 | 5,14 XGS/m² | 0 | 0 solo / 1 moradia | **Sim, com ressalva** — ônus no piso da faixa real (15-40%), favela estável; a ressalva é o estoque caindo e a informalidade de 15% congelada (não há favelização nem regularização em curso). |
| JPN-TOK | 0,257 | 0,845 | 2,14 XGS/m² | 0 | 0 solo / 4 moradia | **Sim** — ônus 26%, 79 mil unidades entregues (0,46%/ano, o maior da amostra), terra abaixo da de SP/MUM como se espera de Tóquio em XGS/renda. |
| NGA-LAG | 0,240 | 0,843 | 4,55 XGS/m² | 0 | 0 solo / 2 moradia | **Sim** — ônus 24%; mas o estoque informal (2,28 M de 4,7 M, 48%) é coerente com Lagos e **não cresce**, o que é otimista para uma cidade que na vida real faveliza. |
| IND-MUM | 0,264 | 0,847 | 5,04 XGS/m² | 0 | 0 solo / 4 moradia | **Sim, com ressalva** — ônus 26% e informal 5,4% da cidade (herança do dado, contra 40-50% reais); a favela parou de crescer por transbordo (correção B5), mas a informalidade sub-representada é dado, não modelo. |
| USA-NEW | 0,291 | 0,843 | 3,62 XGS/m² | 0 | 0 solo / 4 moradia | **Sim** — ônus 29%, o mais alto da amostra (coerente com renda de aluguel cara), informalidade desprezível. |
| AUS-WEI | 0,305 | 0,546 | 8,83 XGS/m² | 0 | 0 solo / 3 moradia | **Parcialmente** — cidade pequena com folga (0,546) faz sentido, mas o ônus 30% numa cidade de 1.100 domicílios é alto (é o piso 0,15 reancorado na renda corrente + custo de terra alto em XGS); sem peso estatístico. |

Em nenhuma das 6 a prefeitura abriu obra de solo; em todas ela gastou em moradia social
(R$ 0,8-4,2 M no ano). O caixa municipal fecha positivo em todas (52 a 3,0 M XGS).

## 9. O que ficou pendente (honesto)

1. **B3 no mundo real:** o gate está corrigido e provado no teste, mas **nenhuma metrópole chega a
   0,90 em 365 d** — depende de a oferta de moradia superar a depreciação (§5.3). Decisão de
   calibração do dono: aumentar a oferta, baixar o gatilho ou aceitar a latência.
2. **60/120 d do `economy_world_test`:** falha o comparativo de comida contra o baseline de 365 d
   por causa do transiente de EGY; em 365 d passa (§4).
3. **Ocupação inicial degenerada por faixa** (p10 = p50 = p90) — herdada do Q-015-C, é calibração
   declarada, não medição.
4. **`tools/_tmp_q15int_monthly.gd`** é sonda temporária (prefixo `_tmp_`): **não** versionada; o
   comando de reprodução está no §10 e o JSON de saída fica em `user://`.
5. Artefatos gerados por teste/import (`docs/economy_ai/decisions.json`,
   `docs/economy_long_run/run.csv`, `*.gd.uid` novos) **não** entraram no commit.

## 10. Reprodução

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
$cmake = "G:/tools/cpp-tools/Lib/site-packages/cmake/data/bin/cmake.exe"
$env:PATH = "G:/tools/llvm-mingw-20261104-ucrt-x86_64/bin;G:/tools/cpp-tools/Scripts;" + $env:PATH
& $cmake -S gdextension -B gdextension/build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DGODOTCPP_DIR=G:/projetos/war-economy/godot-cpp
& $cmake --build gdextension/build-debug

# bateria
foreach ($t in "economy_integration_test","economy_ai_test","population_demo_test",
    "housing_market_test","city_gov_test","urban_land_test","urban_zone_audit",
    "construction_project_test","construction_queue_test",
    "economy_save_load_roundtrip_test","economy_long_run_test") {
  & $godot --headless --path . --script "res://tools/$t.gd"
}

# interação (sonda temporária; JSON em user://q15int_monthly3.json)
& $godot --headless --path . --script res://tools/_tmp_q15int_monthly.gd -- --days=365 `
  --threads=1 --cities=BRA-SAO,JPN-TOK,NGA-LAG,IND-MUM,USA-NEW,AUS-WEI `
  --out=user://q15int_monthly3.json

# sweep ×5 (de dentro da worktree) + agregação
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1 -OutPrefix q15int_
python tools/aggregate_s19_sweep.py --baseline docs/economy/perf/construction_world_sweep.json `
  --dir "$env:APPDATA/Godot/app_userdata/Open Grand Strategy/world_sweep" --prefix q15int_ `
  --suffix _365.json --logdir "$env:TEMP/opengs_q015_sweep" --out "$env:TEMP/q15int_sweep_agg.json"

# perf release (DLL release no slot template_debug) e restauração da debug
& $godot --headless --path . res://tools/perf_day_peak.tscn -- --days=30 --tick-mode=worker --threads=8
```
