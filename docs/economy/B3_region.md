# B3 — Região da cidade = estados inteiros (admin-1)

Fase B3 do plano `docs/plans/perf_pop_financas_plano.md`. Cada cidade passa a ter
**dois blocos**: o **urbano** (a aglomeração da fonte, `simulation_population`) e
a **Região** (toda a população fora da aglomeração = **estados inteiros** admin-1
do Natural Earth). A população simulada salta de **1,32 bi para 8,20 bi** e o
interior (6,14 bi) passa a consumir, trabalhar e poupar. **Muda resultado de
propósito** (recalibração da Parte B).

## 1. Por que estados inteiros

A regra anterior atribuía a cada cidade o *hinterland* por "província → cidade
mais próxima da mesma nação" (`hinterland_2026.json`). A decisão do dono trocou
isso por **estados admin-1 completos**, seguindo a divisa real: a cidade é a
principal do estado; estado sem cidade se junta ao estado vizinho com cidade
(BFS multi-fonte por adjacência; desempate pela maior cidade). Região com nome
real (São Paulo, Bihar, Texas). Dado gerado e validado em
`docs/plans/b3_state_regions.md` (sha `2e375cfb…`, duas execuções idênticas).

## 2. Dado (2026, fonte pública)

- `data/definitions/population/state_regions.json` — **4.226 estados**, **834
  cidades**, 15.547 províncias. Por cidade: `population_region`,
  `urban_population`, `region_population` e `states[]` (ids). Por estado:
  `name`, `nation`, `population_2026`, `area_km2`, `attach_rule`, `city_id`.
  Soma dos estados = soma das regiões = soma de `nations.json` =
  **8.203.501.062**.
- `data/definitions/population/urban_share_reference.json` — urbanização nacional
  (World Bank SP.URB.TOTL.IN.ZS, 2025), **10 nações** (BGD/BRA/CHN/ETH/IDN/IND/
  NGA/PAK/RUS/USA). Sem dado → mix neutro declarado.

O `core/economy/region_data.gd` resolve o bloco `region` de cada cidade e anexa
ao dict antes de `initialize_world` (o snapshot/CLI leva junto).

## 3. Estado e save v18

`City` ganha `Region`/`RegionState` embutidos (um bloco por cidade, sem cidades
novas): nome (estado de maior população), `population_region`/`urban_population`/
`region_pop`, `urban_share`, `employment_mix`, estados (id/nome/população/área),
**5 coortes** (B1) e **5 quintis** (B2) do interior, `cash` e fluxos derivados.

- **Invariante dura:** `city.population = population_region`;
  `city.simulation_population = min(urban_population, population_region)`;
  `region_pop = total − urbano`. Soma global urban+interior = 8.203.501.062
  exatos (urbano capado **2.059.753.933** + interior **6.143.747.129**); **18**
  cidades com região 0 (15 com urbano > estado + 2 com estado vazio — Miyakojima/
  Ponce — + 1 com urbano = estado — SGP-SIN). `reconcile_city_population`
  substitui o rescale por razão da B1: a população exibida cresce com a B1 e com
  a demografia do interior.
- **Save v18:** bloco `city.region` (estado persistente + coortes + quintis do
  interior + caixa). **Migração v17→v18** (mundo real): `region_pop = max(0,
  population − simulation_population)` (dado já no save), coortes/estratos da
  nação e mix neutro — nada inventado. Round-trip sem tick bit-exato
  (`economy_save_load_roundtrip_test` 0 diffs).
- `money_total` inclui `region.cash` (bootstrap `region_pop × kRegionSavingsPerCapita`).
- Binding: `set_city_region` (setup), `get_region_status` (nome/estados/população/
  urbanização/emprego/renda/comida/satisfação) e campos de região em `get_urban_city`.

## 4. Modelo e constantes nomeadas

Tudo é **modelo declarado** em `sim_constants.h`; nenhum número por país foi
inventado. Cadência A6 (`(dia − índice) % período == 0`):

| constante | valor | significado |
| --- | ---: | --- |
| `kRegionUrbanShareNeutral` | 0,50 | urbanização usada sem dado nacional |
| `kRegionAgriAtRural`/`AtUrban` | 0,70 / 0,35 | primário (agro/extração) por urbanização |
| `kRegionIndustryAtRural`/`AtUrban` | 0,15 / 0,25 | indústria leve por urbanização |
| `kRegionRuralWageDiscount` | 0,55 | salário rural < urbano |
| `kRegionLaborShare` | 0,35 | fração da população empregada no primário |
| `kRegionSavingsPerCapita` | 0,50 | poupança inicial (bootstrap) |

- **Diário:** consumo de comida no mercado da cidade-polo (mesmo preço, sem
  frete), depois da comida urbana (o urbano tem prioridade). A compra debita o
  caixa da região e credita o caixa da firma (dono do estoque) — mesmo padrão da
  compra urbana; `market.desired/demand/sold` e `consumed_total/today` são
  aditivos (sem dupla contagem: `phase_reset` zera os fluxos a cada dia).
- **Semanal (`% 7`):** emprego = `region_pop × kRegionLaborShare × mix[0]`;
  salário bruto da semana = `emprego × kFarmWage × kRegionRuralWageDiscount × 7`,
  debitado do caixa da firma (o primário é o empregador); imposto `kTaxRate` à
  prefeitura; o líquido entra no caixa da região. **Publicado em base diária**
  (`wage_income`/`income`/`taxes` = semanal ÷ 7; `*_weekly` guarda o bruto).
- **Mensal (`% 30`):** demografia do interior reaproveitando B1 (nascimentos
  `CBR/1000/12`, mortes `CDR/1000/12` com `kAgeDeathWeight`, envelhecimento
  1/15,1/10,1/20,1/20); `region_pop` = Σ coortes; quintis 20% cada; reconcilia.

**Conservação:** toda transferência tem contraparte (salário ← caixa da firma,
imposto → prefeitura, comida ← caixa da região); `region.cash` = Σ
`stratum_savings` (round-trip exato).

## 5. Números medidos

- **População:** 8.203.501.062 exatos; em 35 dias o interior cresce 6.143.747.129
  → 6.149.449.362 e o urbano 2.059.753.933 → 2.061.195.218 (invariante
  urbano+interior == total mantido).
- **Conservação (8 d):** drift **0.000000000**; `money_total` 6.470.467.553,95.
- **Consumo (dia 1):** `food_spend` 363.819,38 XGS, `food_consumed` 9.095,48 t-eq.
- **Determinismo:** threads 1×16 → mesmo hash do save.
- **Goldens A0/S0 regenerados** (antes → depois):
  A0 `save 84d28700.` → `910bcd9f.` / `status 5f91d311.` → `b84a307c.`;
  `real_60_ai` `d64df2a1./fc560221.` → `c4a75c69./be43d430.`;
  `real_60_noai` `7070ff67./bb88d8bb.` → `7eaa0348./e97d34a8.`;
  `demo_365_noai` `a9dbd376./ab317791.` → `91da28d0./ab317791.` (status **inalterado**
  — o demo AGRIA/PORTUS não tem região). Justificativa: população simulada
  1,32 → 8,20 bi + consumo do interior.
- **CLI × Godot 10 d (IA, threads=1):** byte-idêntico (SHA-256
  `8D8B65E5016C55AE70A72FE66B82C4BC94691114BA33BB004BDE3A16C59C44E7`),
  `money_total` 6.470.467.553,95, 0 diferenças.
- **Sweep 365 d:** falências **1** (real e float; teto 28), drift **−0,0016**,
  hash real `7bec8773.`, float `e0ef98a2.` (difere, como deve). **130 nações fora
  do portão de comida** (avg30) — a dívida de calibração esperada (ver §6).
- **Bench canônico N=3:** dia total **26,43 ms** (mediana), fase `region`
  **0,136 ms** (**0,51%** do dia), variação **28,59%** > 3% (host com
  `godot-editor-mcp` ativo — mesma leitura ruidosa da B2).

## 6. Desvios e dívida de calibração

- **Comida (130 nações fora do portão, pior BHR −1,000):** o interior adiciona
  ~6,14 bi de consumidores ao mercado, mas a oferta de alimento (fazendas) segue
  calibrada para o urbano de ~2,06 bi. O interior **não** tem produção agrícola
  própria nesta fase (o bloco simplificado só compra comida); o
  "agricultor familiar" do plano §B3 fica para a calibração. **Não é erro de
  conservação** — é o item de modelo da recalibração grande (~6,5× consumidores),
  esperado e registrado, sem isenção nova. Piores: BHR −1,000, KWT −0,975, GMB
  −0,942, MRT −0,933, LBY −0,879, SAU −0,874, MNG −0,854, SDN −0,831, CRI −0,829,
  PHL −0,814.
- **Opção 3 (capitais extras) — dívida de dado declarada:** o repositório não tem
  fonte de população urbana para estados sem cidade (GeoNames bruto não está
  versionado) nem `urban_land`/`housing_city` para cidades novas. **Não se
  inventa número.** Falta: (1) a lista de cidades reais (com população urbana da
  mesma fonte GeoNames/ONU) para os estados >20M sem cidade e (2) `urban_land`/
  `housing_city` da cidade nova. Os **15 candidatos** (estado >20M anexado a um
  vizinho, derivados de `state_regions.json`) estão na tabela abaixo. **Dono:
  gerente-dados.**

| estado | país | população | anexado a |
| --- | --- | ---: | --- |
| Henan | CHN | 98.807.834 | Xi'an |
| Hebei | CHN | 73.973.601 | Beijing |
| Hunan | CHN | 65.364.287 | Guangzhou |
| Guangxi | CHN | 49.792.110 | Guangzhou |
| Odisha | IND | 48.251.532 | Kolkata |
| Jiangxi | CHN | 44.779.744 | Guangzhou |
| Jharkhand | IND | 42.050.057 | Kolkata |
| Guizhou | CHN | 38.625.398 | Chengdu |
| Kerala | IND | 37.552.378 | Bengaluru |
| Assam | IND | 37.535.208 | Kolkata |
| Shanxi | CHN | 34.668.260 | Xi'an |
| Haryana | IND | 33.801.450 | New Delhi |
| Chhattisgarh | IND | 31.993.104 | Mumbai |
| Inner Mongol | CHN | 23.897.150 | Xi'an |
| Jilin | CHN | 23.873.808 | Shenyang |

## 7. Reprodução

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
$env:PATH = "G:\tools\cpp-tools\Scripts;G:\tools\llvm-mingw-20261104-ucrt-x86_64\bin;$env:PATH"
cmake --build gdextension/build-debug
& $godot --headless --path . --script res://tools/b3_region_test.gd
# goldens
powershell -NoProfile -File tools/world_equivalence.ps1 -Golden docs/economy/refactor/baseline/A0_60_urban.json -Force -Python G:\tools\cpp-tools\Scripts\python.exe
# CLI × Godot
& $godot --headless --path . --script res://tools/export_world_snapshot.gd -- --out=snap.json --ai=1
& $godot --headless --path . --script res://tools/snapshot_replay.gd -- --in=snap.json --days=10 --out=godot.json --ai=1 --threads=1
gdextension/bin/opengs_sim_cli.exe --snapshot=snap.json --days=10 --out=cli.json --ai=1 --threads=1
# sweep / bench
powershell -NoProfile -File tools/s19_world_sweep_parallel.ps1 -Variants real,float -OutPrefix b3_ -Baseline res://docs/economy/perf/b2_world_sweep.json
powershell -NoProfile -File tools/bench_canonical.ps1 -Rounds 3 -Days 60 -Out docs/economy/perf/b3_main_bench.json
```
