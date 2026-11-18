# B4 — Migração (êxodo rural + cidade → cidade na mesma nação)

Fase B4 do plano `docs/plans/perf_pop_financas_plano.md`. Sobre a região da B3,
a população passa a **mover**: o interior envia migrantes para o polo urbano da
própria cidade (**êxodo rural/urbanização**) e as cidades trocam gente dentro da
mesma nação (**cidade → cidade**). **Muda resultado de propósito** (a
urbanização passa a ser um fluxo, não uma constante). Migração **internacional
fica FORA** desta fase — registrada como futuro.

## 1. Modelo (o que é dado e o que é declarado)

O DADO é o mesmo da B3 (urbanização nacional de `urban_share_reference.json` e
população por estado de `state_regions.json`). Tudo o que segue é **modelo
declarado** em `sim_constants.h`; nenhum número por país foi inventado. A fase
roda na **cadência A6 mensal** (`cadence_due(day, city_index, monthly_period)`),
a mesma agenda da B3, em laço SERIAL determinístico, **depois da moradia** (lê a
vacância e o sinal de aperto do fechamento do dia).

| constante | valor | significado |
| --- | ---: | --- |
| `kMigrationMonthlyMax` | 0,005 | taxa máxima mensal (fração da origem); ~6%/ano no teto |
| `kMigrationGapThreshold` | 0,10 | gap relativo mínimo para disparar o êxodo |
| `kMigrationGapScale` | 0,50 | gap relativo que satura o fluxo |
| `kMigrationAgeWeight[5]` | 0,30/0,80/1,00/0,45/0,10 | peso etário do migrante (15–24 e 25–44 pesam mais) |
| `kMigrationStrataWeight[5]` | 1,00/0,80/0,55/0,35/0,15 | peso de estrato do êxodo (saem primeiro os quintis baixos) |
| `kMigrationServiceWeight` | 0,10 | peso dos serviços na utilidade de cidade → cidade |

- **Êxodo rural** (interior → urbano da MESMA cidade): migra quando a renda
  urbana esperada — `salário × chance de emprego − moradia` (Todaro) — supera a
  renda rural por pessoa (a da B3, salário do primário em base diária) pela
  margem `kMigrationGapThreshold`. O fluxo é `kMigrationMonthlyMax` × fator de
  gap, **limitado pela vaga de moradia no destino** (unidades vazias do mercado
  W3 × pessoas por domicílio). A chance de emprego entra porque a folha só paga
  os empregados: `folha ÷ força de trabalho` já é `salário × emprego` (sem duplo
  desconto).
- **Cidade → cidade** (mesma nação): fonte-cêntrico e determinístico — cada
  cidade, no seu dia, empurra para as cidades de maior **utilidade** (renda
  líquida esperada + serviços) da mesma nação, proporcional ao diferencial e
  limitado pela vaga de cada destino. O perfil de estrato é uniforme (o
  diferencial atrai todas as faixas); só o êxodo enviesa o estrato.
- **Estrutura**: o migrante é jovem (pesos `kMigrationAgeWeight`) e, no êxodo,
  dos quintis mais baixos (`kMigrationStrataWeight`). A **poupança per capita do
  estrato de origem acompanha** o migrante (dinheiro conservado).
- **Conservação**: pessoas (Σ urbano + Σ região invariante; cada movimento é um
  débito atômico na origem + crédito no destino) e dinheiro (`money_total` não
  muda — a poupança sai do caixa da origem e entra no da chegada).

## 2. Estado e save v20

`Region` ganha (persistido, save v20): `migration_rural_in_total`,
`migration_city_in_total`, `migration_city_out_total` (acumuladores desde o dia
0 do save) e os fluxos do último fechamento mensal (derivados, não persistidos):
`migration_month_rural_in/city_in/city_out/net/rate` (`city_in` é o fluxo do
mês do próprio destino: a fase roda em 3 passadas — reset dos fluxos de quem
fecha hoje, migração de todas as fontes, taxa líquida —, então nada que chega no
dia é zerado por outra cidade).

- **Save v20:** bloco `region.migration` (3 acumuladores). **Migração v19→v20:**
  sem o bloco, os acumuladores nascem zerados — nada é inventado para trás (a
  migração conta do dia 0 do save novo). A cadeia v16→v17→v18→v19→v20 é o mesmo
  caminho aditivo.
- `money_total` continua incluindo `region.cash` (a poupança migra de região →
  pop, ambos já contados).

## 3. Status/A4 (sem UI/painel)

- `get_region_status(id).migration`: `rural_in_total`, `city_in_total`,
  `city_out_total`, `month_rural_in`, `month_city_in`, `month_city_out`,
  `month_net`, `month_rate`, `urban_share` (corrente: `simulation_population ÷
  population` = urbano ÷ (urbano + interior), ambos vivos) e `urban_share_ref`
  (World Bank; -1 sem dado).
- `get_status().migration`: totais do mundo + `urbanization` = Σ
  `simulation_population ÷ (Σ simulation_population + Σ region_pop)` — a mesma
  fórmula por cidade. A seed do dia 0 (`region.urban_population` /
  `region.population_region`) não é publicada como valor corrente.
- `month_net = month_rural_in + month_city_in − month_city_out`;
  `month_rate = month_net ÷ (simulation_population + region_pop)`.
- `get_urban_city(id)`: `region_migration_*` e `region_urban_share`.
- Fase de perfil `migration` no bench. Nada de HUD/telas/i18n nesta fase.

## 4. Números medidos

- **Conservação (60 d):** drift **0.000000000**; Σ urbano + Σ interior == Σ
  população (8.214.790.501 no dia 60).
- **Migração em 365 d (mundo, IA + urbano):** `rural_in_total` **21.045**,
  `city_in_total` **37.578**, `city_out_total` **37.578** (entrada == saída,
  conservação de cidade→cidade); urbanização mundial **0,256709 → 0,256210**
  (`get_status().migration.urbanization` bate com o save: 0,256210). A fração é a
  da AGLOMERAÇÃO sobre o total — a região contém o resto do estado, então este
  número é o da B3 (~25%), não o ~57% nacional do World Bank. Por país (dia 0 →
  dia 365): BRA 0,38715→0,38715, USA 0,38463→0,38463, EGY 0,26933→0,26933, CHN
  0,25426→0,25426 (`rural_in` 1.184, `city_in` 99,9), PAK 0,22890→0,22897, IDN
  0,21246→0,21245, NGA 0,20021→0,20018, BGD 0,17787→0,17794, IND
  0,12157→0,12156, ETH 0,05932→0,05932. O fluxo é pequeno porque a vaga de
  moradia em equilíbrio é ~0 (§5); o resto do movimento é demografia.
  Evidência: `docs/economy/perf/b4_urbanization_365.json`
  (`tools/b4_urbanization_365.gd`).
- **Determinismo:** threads 1×16 → mesmo save (b4: `ccc86e24…`; b3 re-verificado
  `e6c3b232…`).
- **CLI × Godot 10 d (IA, threads=1):** byte-idêntico (SHA-256
  `B2D6D066EBE6566621194A8C1776DDFC94C500EB6C79D727C4FEC4AF9C1DF4B9`), 0 diffs.
- **Goldens regenerados (antes → depois):**
  A0 `ff6855a0./3b8749b6.` → `8ec61166./120c1925.`;
  `real_60_ai` `da665be8./652f965d.` → `62d6156f./2d0837ae.`;
  `real_60_noai` `bb35e9b7./165ec560.` → `39164a11./4a20ccd0.`;
  `demo_365_noai` `9f19d27d./489422ae.` → `d189fe63./7fcbb4b2.`
  Justificativa: save v20 (bloco `region.migration`) + status `migration`; a
  migração move gente real (pouca, mas a trajetória muda).
- **Sweep 365 d (real/float, baseline `b3_world_sweep.json`):** falências **11**
  (real) / **12** (float) contra teto 28; drift **−0,0010** nas duas; hash real
  `ad94f030…`, float `172e9e01…` (difere, como deve). Portão de comida (avg30):
  **16** nações pioram no real e **23** no float (vs B3) — a bistabilidade de
  comida dos importadores, NÃO a magnitude da migração (ver §5). Evidência
  versionada: `docs/economy/perf/b4_world_sweep.json`.
- **Bench canônico N=3:** dia total **22,61 ms**, fase `migration` **0,211 ms**
  (**0,93%** do dia; `region` 0,128 ms), variação **1,41% ≤ 3%** (critério OK),
  memória 141,9 MB. `docs/economy/perf/b4_migration_bench.json`.

## 5. Desvios e dívida de calibração

- **Portão de comida (16 real / 23 float pioram vs B3):** a migração move
  ~21 mil pessoas/ano (0,0003% do mundo), mas a comida dos importadores é
  **bistável** (mesmo mecanismo da B1/B2/B3: poupança → 0, preço do trigo ~3×,
  consumo cai). Uma trajetória minimamente diferente (qualquer pessoa a mais no
  urbano) derruba os importadores marginais (AGO 0,890→0,524, ARE 0,445→0,063).
  Não é erro de conservação e **não é a magnitude da migração** — é a lacuna de
  financiamento do comércio (Parte C), registrada sem isenção nova.
- **Vacância ≈ 0 no baseline:** no mundo em equilíbrio (W0), a vacância começa 0
  e só aparece onde a população ENCOLHE (países velhos) ou o estoque sobra —
  por isso a migração no mundo real é pequena. O **teste** cria vaga (infla o
  estoque) para provar direção/limite/idade; no caso de limite ele afirma que a
  vaga disponível no fechamento **é menor que a taxa máxima do mês** e que o
  fluxo recebido **é igual à vaga disponível** (2.613 pessoas contra taxa máxima
  de 156.005) — sem a moradia, o fluxo teria sido até ~60× maior. A calibração
  da oferta de moradia (§19) destrava a urbanização real.
- **Urbanização simulada ≠ referência:** o "urbano" do núcleo é a aglomeração da
  fonte (~2,1059 bi no dia 0, 2,1197 bi no dia 365); o resto do estado
  (incluindo a área urbana fora do roster) vive na região. A urbanização
  publicada (~0,2562) é a fração da AGLOMERAÇÃO, não o ~57% nacional — herança
  da B3, não da B4.

## 6. Reprodução

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
$env:PATH = "G:\tools\cpp-tools\Scripts;G:\tools\llvm-mingw-20261104-ucrt-x86_64\bin;$env:PATH"
cmake --build gdextension/build-debug
& $godot --headless --path . --script res://tools/b4_migration_test.gd
# goldens
powershell -NoProfile -File tools/world_equivalence.ps1 -Golden docs/economy/refactor/baseline/A0_60_urban.json -Force -Python G:\tools\cpp-tools\Scripts\python.exe
& $godot --headless --path . --script res://tools/economy_refactor_equiv.gd -- --mode=world --days=60 --ai=1 --out=docs/economy/refactor/baseline/real_60_ai.json
# CLI × Godot
& $godot --headless --path . --script res://tools/export_world_snapshot.gd -- --out=snap.json --ai=1
& $godot --headless --path . --script res://tools/snapshot_replay.gd -- --in=snap.json --days=10 --out=godot.json --ai=1 --threads=1
gdextension/bin/opengs_sim_cli.exe --snapshot=snap.json --days=10 --out=cli.json --ai=1 --threads=1
# sweep / bench
powershell -NoProfile -File tools/s19_world_sweep_parallel.ps1 -Variants real,float -OutPrefix b4_ -Baseline res://docs/economy/perf/b3_world_sweep.json
& $godot --headless --path . --script res://tools/b4_urbanization_365.gd
powershell -NoProfile -File tools/bench_canonical.ps1 -Rounds 3 -Days 60 -Out docs/economy/perf/b4_migration_bench.json
```

## 7. Correções da revisão de entrega (antes do merge)

- **Urbanização publicada era a seed do dia 0:** `get_status().migration.urbanization`
  somava `region.urban_population` e `get_region_status().migration.urban_share`
  dividia `region.urban_population ÷ region.population_region`. Agora ambos usam
  a população **viva** (`simulation_population ÷ population`, isto é, urbano ÷
  (urbano + interior)); o teste de conservação confere a igualdade com Σ das
  cidades e o mundo em 365 d bate com o save
  (`b4_urbanization_365.json`, `world_status_urbanization_end`).
- **Limite de moradia não era vinculante:** o caso (f) agora calcula a vaga
  disponível em PESSOAS pela mesma conta do núcleo (`urban_vacancy_people`),
  afirma que ela é **menor que a taxa máxima do mês** e que o fluxo é
  **exatamente** a vaga disponível — o fluxo é cortado pela moradia, não pela taxa.
- **Fluxo do mês do destino:** a fase roda em 3 passadas (reset → migração →
  taxa) e o destino acumula `month_city_in`; `month_net` inclui a entrada.
- **Evidência do sweep 365 d versionada** (`docs/economy/perf/b4_world_sweep.json`)
  e a urbanização inicial×final passou a ter ferramenta própria
  (`tools/b4_urbanization_365.gd`) e evidência
  (`docs/economy/perf/b4_urbanization_365.json`).
