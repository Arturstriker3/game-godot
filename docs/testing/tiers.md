# Validação em camadas — quick / pr / nightly

Objetivo: validar uma etapa antes do merge em **15–25 min**, com as sentinelas que
pegam os erros caros. A bateria completa e as rodadas longas passam a ser noturnas.

Fonte dos tempos: `docs/testing/test_inventory.md` (bateria de 24/11 05:22) e a
medição abaixo. O runner continua sendo `tools/run_tests.ps1`; ele mantém o lock
global (`G:\opengs_tmp\opengs_godot_test.lock`), um Godot por vez, timeout e
artefatos em `G:\opengs_tmp`.

## Camadas

| tier | o que roda | quando usar | tempo observado |
|---|---|---|---|
| `quick` | testes impactados pelo `git diff` + sentinelas baratas | antes de cada commit/etapa | 1–12 min conforme o diff |
| `pr` | `quick` + rodada c1 de **90 dias** com digest (`c1_world_ai_365.gd --days=90`) | antes do merge | 4–20 min |
| `nightly` | `-All` (102 testes) + rodada c1 de **365 dias** | madrugada/CI noturna | bateria completa + ~3 min |

```powershell
# pre-merge normal: pega o diff contra balance/global-resources (inclui nao commitado)
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Tier pr

# so ver o que rodaria (nao pega lock, nao abre Godot)
powershell ... -Tier quick -DryRun

# outra base (branch, tag, commit)
powershell ... -Tier quick -Base main

# sem tier: comportamento antigo, inalterado
powershell ... -All
powershell ... -List economy_integration_test
```

- `-Tier` usa `tools/test_impact_map.json`. `-List`/`-Pattern` continuam valendo:
  com tier, a seleção explícita é somada às sentinelas.
- O diff considerado é `<Base>...working tree` + arquivos não rastreados
  (`git ls-files --others`). Ruído de import/build (`.uid`, `.import`, `.godot/`,
  `gdextension/bin/`, `gdextension/build*/`) é ignorado.
- Caminho desconhecido cai em `broad_economy` (conservador). Núcleo central
  (`sim_core`, `sim_day`, `world_state`, `save_*`, `commands`, `actions`,
  `economy_sim`) seleciona tudo e avisa que a cobertura completa pede `nightly`.
- Qualquer arquivo em `gdextension/src/` (não só os 9 centrais) seleciona o conjunto
  amplo de núcleo (`core_set`) e imprime
  `[tier] aviso: mudança no núcleo: rode -Tier nightly antes do merge se possível`.
- Alterar a própria infra de testes (`tools/run_tests.ps1`, `tools/test_impact_map.json`
  ou `tools/tmp_paths.gd`) invalida a seleção: o mapa devolve `ALL`, ou seja, a bateria
  inteira, equivalente a `-All` (vale também no `quick`). Editar qualquer outro
  `tools/<nome>_test.gd` continua selecionando só aquele teste.
- O `summary.json` ganhou `tier`, `base`, `changed_files` e `selection`
  (teste → arquivos que o selecionaram). Campos antigos não mudaram.

## Sentinelas (sempre executadas em quick e pr)

| teste | ~tempo | o que prova |
|---|---:|---|
| `economy_save_load_roundtrip_test` | 27 s | determinismo save/load: ticar direto == salvar/ticar/carregar/ticar |
| `c3_bank_seizure_conservation_test` | 13 s | conservação exata de dinheiro na execução do calote (C3/C4) |
| `c1_balance_test` | 22 s | identidade financeira diária, conservação de caixa, patrimônio B6/G2 e round-trip v27 |
| `c1_world_ai_90` (`run`) | 39 s | mundo real com IA por 90 d: invariante financeiro, comida, balanço e digest da trajetória |

A rodada de 90 dias é a sentinela de `pr` (caro demais para todo commit). A de 365
dias é a sentinela de `nightly`. A lista fica em `sentinels` no mapa.

## Mapa de impacto (`tools/test_impact_map.json`)

- `test_sets`: conjuntos nomeados (`broad_economy`, `bank_fast`, `production`,
  `urban_full`, `population_full`, `world_data`, `ui`, `map`, `game`, `core_set`),
  com referência entre conjuntos.
- `rules`: primeira regra que casa com o caminho vence. Exemplos conservadores:
  - `gdextension/src/sim/(save|sim_core|sim_day|...)` → `ALL`;
  - qualquer outro `gdextension/src/**` → conjunto amplo de núcleo (`core_set`) + aviso de `nightly`;
  - `tools/run_tests.ps1|tools/test_impact_map.json|tools/tmp_paths.gd` → `ALL`;
  - `core/economy/bank|fx|monetary|exchange|stock` → bancos rápidos + estresse de câmbio + C1;
  - `data/definitions/economy/(recipes|goods|production)` → cadeia de produção + D-125/D-127;
  - `^docs/` e `*.md` → só sentinelas.
- `default_tests` → `broad_economy` quando nenhuma regra casa.

Editar o mapa é barato; os nomes são validados contra o catálogo na execução
(nome fora do catálogo gera aviso, não derruba a bateria).

## Medição de `-Tier pr` (diff de exemplo: `data/definitions/economy/recipes.json`)

Data: 24/11/2026. Worktree `G:\projetos\opengs-wt-testinfra`, base `8caf1cf1`.
Núcleo recompilado nesse commit (`RelWithDebInfo`) — o `gdextension/bin` não é
versionado e o binário copiado do repo principal era de 22/11 (anterior a D-125/D-127).

- Entradas: **13** (12 testes + `c1_world_ai_90`), todas **OK**.
- Parede total: **252,6 s** (4,2 min), incluindo a espera curta pelo lock de outro
  runner e a importação já quente (overhead de ~5 s). Soma dos testes: 247 s.
- Testes: `c1_balance`, `c3_bank_seizure_conservation`, `d125_efficiency`,
  `d127_byproducts`, `economy_integration`, `economy_long_run`,
  `economy_save_load_roundtrip`, `economy_world`, `goods_data`,
  `population_demo`, `price_formation`, `production_chain` + `c1_world_ai_90`.
- O diff também continha `docs/testing/test_inventory.md` e dois JSON/CSV em `docs/`
  que os próprios testes regravam (ruído; foram restaurados depois).
- Worktree novo (sem `.godot`): some ~2–4 min de `--import` na primeira execução.

Observação de ambiente: a bateria inventariada (98,8 min) rodou com um DLL debug
antigo de 58 MB. Com o núcleo recompilado no mesmo commit, os mesmos testes rodam
~2x mais rápido (ex.: `d125` 35 s → 14 s; `c1_balance` 62 s → 22 s). Os tempos do
inventário são um teto conservador.

## CLI sem Godot (status e o que falta)

Existe executável do núcleo: `gdextension/src/cli/main.cpp` → `opengs_sim_cli
--snapshot in.json --days N --out out.json [--threads T] [--ai 0|1]`. A
equivalência CLI×Godot já existe em `tools/world_equivalence.ps1`. O runner ganhou
o modo de checagens de CLI (`cli_checks` no mapa + `-Parallel N`, padrão 3,
**somente CLI**; validado com 2 processos de 1,2 s terminando em 1,57 s) e ignora
checagens cujos `requires` não existem.

Não foi possível migrar 2–3 testes agora, e o motivo é concreto:

1. O CLI não tem harness de asserções: ele roda o mundo e grava o save. Quem
   compara é o script externo de equivalência.
2. O CLI recusa o cenário demo: `initialize_scenario()` não é `world_mode`, então
   `seed_industry()` é no-op e o save fica com `industry_seeded=false`
   (erro: "snapshot não é um mundo inicializado").
3. O snapshot do mundo real (`export_world_snapshot.gd`) tem dezenas de MB, grande
   demais para versionar como fixture.

Candidatos quando (1)–(3) forem resolvidos: `c1_balance_test`,
`c3_bank_seizure_conservation_test`, `price_formation_test`,
`production_chain_test`, `population_demo_test`. O caminho mais curto é um modo
`--fixture`/demo no CLI (núcleo, fora deste escopo) ou um formato de snapshot
compacto. Até lá, `cli_checks` fica vazio (`[]`) e nada é pulado à toa.

## Propostas de encurtamento (fase 2 — não implementadas)

Não mexa nos testes lentos agora; a fase 1 só os tira do caminho rápido.

| teste | hoje | o que confere | proposta |
|---|---:|---|---|
| `c2c_ab_365_test` | 781 s | A/B de 3 mundos ×365 d no mesmo processo | braço de 90 d no `pr`; 365 d só `nightly` |
| `economy_bistability_test` | 320 s | biestabilidade de liquidez/fome com estoque | aceitar `--days`; 90 d no quick, 365 no nightly |
| `c2b2_fx_stress_test` | 275 s | estresse de câmbio 365 d com reservas mínimas | 120 d de fumaça por padrão; 365 d sob flag |
| `construction_queue_test` | 272 s | moradia/prédio/infra na mesma fila | dividir por cenário e rodar o cenário tocado |
| `city_gov_test` | 214 s | prefeitura (W2 §19) | reduzir horizontes por aceite; manter 1 aceite longo |
| `b6_property_test` | 210 s | patrimônio, anel, Gini, conservação | separar dado/estado (rápido) do horizonte longo |
| `strata_test` | 204 s | estratos/quintis com patrimônio | idem: contrato rápido + 1 janela longa noturna |
| `b4_migration_test` | 187 s | êxodo rural/cidade→cidade | janela curta para conservação; longa noturna |
| `agriculture_seasonal_test` | 184 s | safra sazonal (D-096) | 1 ano agrícola é o mínimo; deixar no nightly |
| `agri_calendar_owner_test` | 154 s | calendário herdado da região (D-098) | idem |
| `c2_central_bank_test` | 148 s | banco central por moeda | separar estado (rápido) da medição de 365 d |
| `b7_informality_test` | 146 s | informalidade, mortalidade, save v24 | contrato rápido + janela longa noturna |

Padrões comuns: (a) `--days` configurável com default curto no quick e 365 no
nightly; (b) separar "contrato de dado/estado" (segundos) de "trajetória longa"
(minutos); (c) cachear a construção do mundo real entre testes do mesmo run, se o
runner passar a rodar em lote.
