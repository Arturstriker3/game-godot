# A0 — Benchmark canônico e equivalência (como rodar)

> Fase A0 de `docs/plans/perf_pop_financas_plano.md` (plano aprovado pelo dono
> em 2026-11-17; ele vive no worktree principal, não neste branch). O baseline
> versionado é `baseline_A0.json` — medição oficial de 60 d × 3 rodada em
> 2026-11-17 sobre o Q-015 integrado (ver §6) — e esta pasta guarda a ferramenta
> de equivalência das fases A1–A5 e as medições históricas (`f*_*.txt`). Nada
> aqui é número inventado: o que não foi medido aparece como "não medido".

## 1. O que já existia (não duplicar)

| ferramenta | mede | build que serve |
|---|---|---|
| `gdextension/src/sim/profiling.{h,cpp}` | fase do tick C++ (`OGS_SIM_ZONE`/`OGS_SIM_PHASE_LAP`), em µs por fase e por dia | `OGS_PROFILING` ligado |
| `tools/perf_day_peak.tscn/.gd` | **caminho real do dia**: tick no worker, `poll()` da main, enfileirar pedidos, `ai.plan`, IA fatiada por frame, mapa (opcional), virada, frame de pico, memória estática | debug (fases) ou release (parede) |
| `tools/real_tick_profile.gd` | só o tick C++, síncrono, pelo autoload (cenário real, sem mapa) | debug |
| `tools/world_scale_bench.tscn/.gd` | mundo **sintético** (N cidades): tick + fases, status, save/load, determinismo síncrono × assíncrono | debug |
| `tools/ai_scale_bench.gd` | custo da IA (`plan`/`pump`) por número de cidades | qualquer |
| `tools/economy_world_test.gd` | mundo real 60/365 d com IA e `--urban=1`: comida, trabalho, setores, propriedade; `--out` = relatório agregado (não é dump do mundo) | qualquer |
| `tools/economy_world_sweep.gd` + `tools/s19_world_sweep_parallel.ps1` | sweep 365 d em 5 variações de câmbio, em paralelo (1 processo cada) | qualquer |
| `tools/economy_refactor_equiv.gd` | **golden S0**: 60 d, `save_sha256` + `status_sha256` contra `docs/economy/refactor/baseline/*.json` (sem urbano) | qualquer |
| `tools/geography_load_profile.gd`, `city_render_benchmark.gd`, `map_scale_bench.gd`, `camera_pan_bench.gd` | boot da geografia e render (fora do escopo de A0) | — |

As medições "release" antigas (`docs/economy/perf/f*_release_run*.txt`,
`f*_perf_release.txt`) foram feitas **à mão**: copiar a DLL release sobre o slot
`template_debug` e rodar `perf_day_peak`. Não havia script; agora há
(`tools/bench_canonical.ps1`), com restauração da DLL conferida por hash.

Até aqui o release **não** tinha as fases internas: `profiling.h` é compilado com
`OGS_PROFILING` só em Debug (`s2_profile.md` §2 registra "fases internas ficam
0,0 por desenho"). Por isso o A0 adiciona um build de **release de medição**:

```
-DOGS_PROFILE_RELEASE=ON   # OGS_PROFILING também em Release; padrão OFF
```

O release publicado continua sem perfil (custo zero). O release de medição paga
um relógio por fase por dia — é o único jeito de ter "tempo por fase" em release
sem mudar regra de simulação.

## 2. Build necessário (as duas DLLs)

Na raiz do worktree, com o cmake/ninja/clang do projeto:

```powershell
$cmake = "G:/tools/cpp-tools/Lib/site-packages/cmake/data/bin/cmake.exe"
$env:Path = "G:/tools/cpp-tools/Scripts;G:/tools/llvm-mingw-20261104-ucrt-x86_64/bin;" + $env:Path

# release de medição (perfil ligado) -> gdextension/bin/...template_release...
& $cmake -S gdextension -B gdextension/build-profile-release -G Ninja `
    -DCMAKE_BUILD_TYPE=Release -DGODOTCPP_TARGET=template_release `
    -DOGS_PROFILE_RELEASE=ON -DGODOTCPP_DIR=<godot-cpp>
& $cmake --build gdextension/build-profile-release --parallel 6

# debug real (é o que o Godot carrega e o que o bench troca/restaura)
& $cmake -S gdextension -B gdextension/build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGODOTCPP_DIR=<godot-cpp>
& $cmake --build gdextension/build-debug --parallel 6
```

`-DGODOTCPP_TARGET=template_release` é obrigatório: sem ele o godot-cpp compila
com `DEBUG_ENABLED` e a DLL sai no nome `template_debug` (medição não é a do
release). As duas DLLs em `gdextension/bin/` são ignoradas pelo git.

## 3. Rodar o bench (A0)

```powershell
# oficial: 60 d, 3 rodadas sequenciais
powershell -NoProfile -File tools/bench_canonical.ps1

# smoke (prova que funciona, não é baseline)
powershell -NoProfile -File tools/bench_canonical.ps1 -Days 5 -Rounds 1 -Out "$env:TEMP\a0_smoke.json"
```

Condições (o script aborta se não valerem):

- **Editor Godot fechado.** Qualquer processo `Godot*` com janela → aborta com
  os PIDs e **não mata nada**. Outro Godot headless ativo gera aviso (rouba CPU
  e o número não vale).
- **Nada mais pesado rodando** (sweep, build, captura). A medição é de parede.
- Threads **não** são passadas: o núcleo usa o padrão dele (mesma condição das
  medições históricas).
- Cenário real: 834 cidades, 179 nações, 113 produtos, IA ligada, `--urban=1`
  (o autoload monta o mundo; o bench não mexe em `core/`).

O que ele faz: confere o hash do slot `template_debug`, copia a release de
medição por cima, roda `perf_day_peak` N vezes em processos novos, e **sempre**
restaura a debug no `finally` conferindo o SHA-256 (falha de restauração → exit
3, com o caminho do backup). Saída: `docs/economy/perf/baseline_A0.json`:

- `rounds[]`: por rodada, `metrics` (tick de parede, poll, enfileirar, `ai_plan`,
  IA fatiada, mapa, virada, pico, `day_total_ms` = tick + plano + IA fatiada) e
  `phases` (fase do tick), cada um com média/p50/p95/máx/n; `memory_static_mb`.
- `aggregate`: mediana entre rodadas; `variation_pct`: (máx−mín)/|mediana|.
- `criterion`: variação ≤ 3% em `tick_wall_ms` e `day_total_ms` (as métricas-chefe;
  as demais são publicadas por referência — `poll_ms` e fases zeradas oscilam em
  µs e reprovariam qualquer bench).

## 4. Equivalência das fases A1–A5

Critério comum: **mundo idêntico campo a campo** ao pré-mudança, mais a bateria
de economia verde. Dois níveis:

**Nível 1 — golden S0 (barato, versionado).** `tools/economy_refactor_equiv.gd`
compara `save_sha256` + `status_sha256` de 60 d contra
`docs/economy/refactor/baseline/real_60_ai.json`. É o gate histórico; não cobre
`--urban=1` e não aponta o campo divergente.

**Nível 2 — A0 (campo a campo, com urbano).**

```powershell
# 2 rodadas no mesmo commit -> 0 diferenças (determinismo)
powershell -NoProfile -File tools/world_equivalence.ps1

# contra o golden A0 (só shas + config; barato, não lê o dump)
powershell -NoProfile -File tools/world_equivalence.ps1 -Rounds 1 `
    -Baseline docs/economy/refactor/baseline/A0_60_urban.json

# contra um dump de referência (relata o 1º caminho divergente)
powershell -NoProfile -File tools/world_equivalence.ps1 -Rounds 1 -Baseline <dump.json>

# (re)gerar o golden A0 — só com aval do gerente
powershell -NoProfile -File tools/world_equivalence.ps1 -Golden docs/economy/refactor/baseline/A0_60_urban.json
```

`tools/world_equivalence.gd` roda 60 d (IA, `--urban=1`, seed fixa = 0, threads
no padrão do núcleo) e grava um JSON canônico: `meta` (inclui os dois shas),
`save` (`demo_to_dict()`) e `status` (`get_demo_status()`), com chaves ordenadas
e precisão total. `tools/world_equivalence_compare.py` compara dois desses dumps
campo a campo e imprime o primeiro caminho divergente (`save.world.cities[12]...`)
com os dois valores; floats com `--tolerance` (padrão 0 = exato), `--ignore`
para ruído de relógio. Os dumps têm ~110 MB: ficam em `%TEMP%`, **não** vão para
o git; o golden versionado é só o `meta` (shas + config).

`python` do PATH neste ambiente é o atalho da Microsoft Store (falha ao
executar): passe `-Python C:\caminho\python.exe` para o `world_equivalence.ps1`.

Regras da comparação:

- Rode sempre **a mesma DLL/configuração** nos dois lados (release × release ou
  debug × debug). Mudar de build pode mudar o último bit de um float.
- `meta.generated_utc` é relógio de parede e fica fora da comparação.
- Seed: o núcleo não tem RNG; a "semente fixa" é o registro do `--seed=0` no
  `meta`, sem perturbação de câmbio (a de `economy_world_test` é outra coisa).
- Qualquer fase A1–A5 fecha com **0 diferenças** no nível 2 e bateria verde;
  A6 é a única que pode mudar o mundo (e aí regenera golden com parecer).

## 5. Validação de 2026-11-17 (smoke, não é baseline)

Commit `f4e70a4` + esta ferramenta. DLL release de medição
`sha256 dae1cb4f…76ff18af` (1.748.992 B), slot debug restaurado com hash igual
antes/depois em todas as execuções.

Bench (`-Days 5 -Rounds 1`, 834 cidades, IA, urbano, threads padrão; 5 dias é
mundo jovem — os números crescem com as construções, então **não** servem de
baseline):

| métrica | média | p50 | p95 | máx |
|---|---:|---:|---:|---:|
| tick de parede (worker) | 29,90 | 28,01 | 35,70 | 35,70 ms |
| fase `status` | 11,78 | 11,85 | 13,35 | 13,35 ms |
| IA planejar | 4,10 | 4,17 | 4,25 | 4,25 ms |
| IA fatiada/dia | 51,40 | 51,02 | 54,54 | 54,54 ms |
| dia completo (tick+IA) | 85,40 | 83,29 | 90,71 | 90,71 ms |
| virada da main | 9,15 | 9,52 | 10,55 | 10,55 ms |

Memória estática 280,0 MB (sem mapa 3D). Fases internas ≠ 0 confirmam o build
com `OGS_PROFILE_RELEASE=ON`; o script aborta se vierem zeradas.

Equivalência (60 d, IA, `--urban=1`, seed 0): golden
`docs/economy/refactor/baseline/A0_60_urban.json`
(`save_sha256 373d97d7…a79a6e894`, `status_sha256 aea1f594…3be5cb49`,
money_total 2.200.539.587,40, 0 falências, 54.553 negócios de serviço) e duas
rodadas comparadas **campo a campo → 0 diferenças** (~26 s por rodada, 53 s no
total). O dump de cada rodada tem ~110 MB e fica em `%TEMP%`.

O golden foi gerado com a release de medição. Comparação com outra build
(debug × release) não foi medida: a regra é comparar **mesma build** dos dois
lados; se precisar do caminho debug, regenere o golden com a DLL debug e
registre o `sha` dela. **Atualização (2026-11-17, Q-015 integrado):** o golden
foi regenerado com a **DLL debug real** do HEAD (ver §6) — os shas acima são
históricos.

## 6. Baseline oficial A0 (2026-11-17, Q-015 integrado)

Medição oficial: `tools/bench_canonical.ps1` (60 d × 3 rodadas sequenciais,
editor Godot fechado, CPU ociosa), saída em `docs/economy/perf/baseline_A0.json`.

- **HEAD:** Q-015 integrado + plano do dono (branch `balance/global-resources`).
- **DLL release de medição** (`OGS_PROFILE_RELEASE=ON`, rebuilt do HEAD):
  `sha256 f825a7b4…9dbc851d`. Slot debug restaurado com `sha256 62f36398…209c59b`
  (a mesma DLL do golden de equivalência).
- **Host:** AMD Ryzen 7 5700X3D, 16 threads lógicos, Windows 11 Pro. Threads não
  passadas (padrão do núcleo); memória estática 314,5 MB sem mapa 3D.

| métrica (mediana entre rodadas) | ms | variação entre rodadas |
|---|---:|---:|
| tick de parede (worker) | 36,58 (p95 por rodada 41,75 / 42,07 / 42,15) | 1,96% |
| dia completo (tick + IA) | 155,51 (p95 168,5 / 174,0 / 173,3) | 1,18% |
| fase `status` | 15,86 (`status_cities` 11,17 + `status_orders` 3,50) | 1,13% |
| `trade` | 6,27 (`trade_food` 5,47) | 1,04% |
| `arrival` | 5,48 | 4,71% |
| IA planejar (`ai_plan`) | 4,91 | 1,60% |
| IA fatiada/dia (`ai_pump`) | 113,94 | 0,98% |
| virada da main (`turn`) | 10,45 | 1,37% |
| `pump` (total por dia) | 353,15 | 4,49% |

**Critério A0:** variação ≤ 3% em `tick_wall_ms` e `day_total_ms` →
**máx 1,96% — OK**. `frame_peak_ms` variou 7,54% (não é métrica-chefe: oscila em
µs de frame) e as fases internas de µs abaixo de ~1 ms variam 5–11%.

**Equivalência no mesmo HEAD:** golden `docs/economy/refactor/baseline/A0_60_urban.json`
regenerado com a DLL debug (`save_sha256 3e3ebbbe…1a3d0b7`,
`status_sha256 d79bfe32…c95ac9b`, money 2.200.539.587,40, 0 falências,
54.554 negócios de serviço); 2 rodadas comparadas campo a campo → **0 diferenças**
e 1 rodada contra o golden → shas iguais.
