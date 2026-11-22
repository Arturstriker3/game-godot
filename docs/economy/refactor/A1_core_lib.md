# A1a — núcleo como biblioteca C++ pura + executável de linha de comando

> Fase **A1a** de `docs/plans/perf_pop_financas_plano.md` (Parte A). Branch
> `perf/a1-core-lib`, saída de `balance/global-resources` (`d71deb5`, HEAD do
> baseline A0). Escopo desta fatia: separar o núcleo do engine e dar a ele um
> executor próprio. **Não muda nenhum número** — a regra é equivalência
> funcional com o A0.

## 1. O que mudou de lugar

### Inventário (A1a-1): o que dependia do Godot

Antes de mover qualquer coisa, o inventário de `src/sim/` e `src/edge/`:

- `src/sim/` (**30 arquivos**, ~8.300 linhas): **zero dependência** — nenhum
  `#include` do godot-cpp, nenhum símbolo `godot::`, nenhum uso de
  `String`/`Dictionary`/`Array`/`Variant`. As 16 menções a `godot_cpp` no
  diretório são comentários-guarda do gate §2.2 ("nada de godot_cpp").
- `src/edge/save_load.h` + `save_load.cpp` (**2.242 linhas**): **os únicos
  dependentes** — 1 e 3 `#include <godot_cpp/...>` e todo o save montado sobre
  `String`/`Array`/`Dictionary`/`Variant`.
- `src/economy_sim.{h,cpp}`: a fachada (binding, status, `demo_set_*`,
  `demo_build`, `demo_seed_industry`) — fica no adaptador por desenho.
- Fora isso, só citação em comentário (`scenario_fixture.h`, `profiling.h`).

Ou seja: o núcleo já era STL puro; o único ponto a desacoplar era o
**save/load**, e é ele que puxava o tipo de valor do Godot para dentro da
biblioteca.

### Os três artefatos

Agora existem **três artefatos**:

| artefato | o que é | depende do Godot? |
|---|---|---|
| `opengs_sim` (lib estática) | todo o núcleo: `sim/*.cpp` (fases, mundo, save, definições, ordem do dia) | **não** |
| `economy_gdextension` (DLL) | adaptador fino: `register_types`, a classe `EconomySim` (binding + status) e a ponte de valores | sim |
| `opengs_sim_cli` (exe) | roda N dias do tick a partir de um snapshot JSON | **não** |

Arquivos novos no núcleo (`gdextension/src/sim/`):

- `save_value.{h,cpp}` — `ogs::save::Value`: objeto/array/string/número/bool com
  **tipo explícito** e **ordem de inserção preservada** (o hash do save do jogo
  depende dela), mais `parse_json_text/file` e `dump_json`. O `nlohmann/json`
  fica confinado ao `.cpp`.
- `save_state.{h,cpp}` — porte **mecânico** de `edge/save_load.cpp` (save v14:
  33 chaves de topo, mesma ordem, mesmas expressões). Só mudaram includes,
  namespace e os `Dictionary x;`/`Array x;` do serializador, que passam a nascer
  como contêiner vazio explícito (`Value::make_object/array`) para que contêiner
  vazio saia `{}`/`[]` como no Godot (senão o `save_sha256` mudaria).
- `save_definitions.{h,cpp}` — o bloco **estático** que o save não carrega:
  produtos/preços-base/setores, receitas, níveis de necessidade, setores de
  serviço, âncoras de emprego e os parâmetros de balanceamento. É o que os
  `demo_set_*` da fachada montam a partir de `data/definitions/`.
- `sim_day.{h,cpp}` — `ogs::demo_tick_day(WorldState&, SimCore&)`: a ordem do dia
  (prólogo → urban_land::phase_month → arrival → reset → fleet → labor →
  production → raw → income → housing → services → trade → accounting → prices →
  clamp → fiscal → fx → invariants), sem lock, sem log, sem status.
- `gdextension/third_party/nlohmann/` — `json.hpp` 3.11.3 + `LICENSE.MIT`.

Na fachada (`gdextension/src/`) sobraram `economy_sim.cpp` (binding + status),
`register_types.cpp`, `edge/value_bridge.{h,cpp}` (`godot::Variant` ⇄
`save::Value`) e `edge/save_load.{h,cpp}`, agora **22 linhas**: as assinaturas
antigas `to_dict`/`from_dict` seguem existindo e só delegam para o núcleo.

Superfície nova de ferramenta (aditiva, não usada pelo jogo):
`demo_to_snapshot()` / `demo_from_snapshot(dict)` e as variantes de arquivo
`demo_to_snapshot_file(path)` / `demo_from_snapshot_file(path)`.

## 2. Como compilar

Mesmo toolchain e mesmas flags do A0 (clang do llvm-mingw, ninja, C++20, sem
`fast-math`; `OGS_PROFILING` segue só em Debug ou com `OGS_PROFILE_RELEASE=ON`).

```powershell
$env:Path = "G:/tools/cpp-tools/Scripts;G:/tools/llvm-mingw-20261104-ucrt-x86_64/bin;" + $env:Path
$cmake = "G:/tools/cpp-tools/Lib/site-packages/cmake/data/bin/cmake.exe"
& $cmake -S gdextension -B gdextension/build-debug -G Ninja `
    -DCMAKE_BUILD_TYPE=Debug -DGODOTCPP_DIR=G:/projetos/war-economy/godot-cpp
& $cmake --build gdextension/build-debug --parallel 8
```

Saídas: `gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll`
e `gdextension/bin/opengs_sim_cli.exe` (o `opengs_sim_cli` também é construído
no configure de Release; ele linka `-static` no MinGW).

## 3. O snapshot

O snapshot é o **estado completo** do mundo do núcleo em JSON, com duas chaves:

```json
{ "definitions": { ... }, "save": { ... } }
```

- `save` é exatamente o dicionário do save v14 (`save::to_value`) — nada novo.
- `definitions` cobre o que o save historicamente não carrega (vem dos
  `demo_set_*`): é o que permite ao CLI reconstruir o mundo **sem** ler
  `data/definitions/` nem o engine.

Produção (mundo montado como o jogo, **IA desligada**, `--urban=1`, com o
bootstrap industrial aplicado — sem ele o mundo exportado não é o estado de
partida real e o CLI recusa):

```powershell
& $godot --headless --path . --script res://tools/export_world_snapshot.gd -- `
    --out=%TEMP%\a1_snapshot.json
```

Replay no Godot a partir do mesmo arquivo (é o par de comparação do CLI):

```powershell
& $godot --headless --path . --script res://tools/snapshot_replay.gd -- `
    --in=%TEMP%\a1_snapshot.json --days=60 --out=%TEMP%\godot60.json
```

## 4. Como rodar o CLI

```powershell
gdextension/bin/opengs_sim_cli.exe --snapshot=%TEMP%\a1_snapshot.json `
    --days=60 --out=%TEMP%\cli60.json [--threads=0]
```

- `--threads` usa o mesmo contrato de `demo_set_sim_threads` (0 = automático,
  1 = serial).
- Saída: o mesmo formato de snapshot (`definitions` + `save`), sem `meta`, com
  `dump_json(..., 2)` (UTF-8 sem BOM, chaves na ordem de inserção).
- Exit 0 ok, 1 violação de invariante, 2 erro de uso/entrada.
- stderr: `[cli] dias=.. threads=.. day=.. cidades=.. ms_total=.. ms_dia=..
  money_total=..` — atenção: `ms_dia` inclui a carga do snapshot.

Comparação campo a campo (mesma ferramenta do A0):

```powershell
python tools/world_equivalence_compare.py %TEMP%\cli60.json %TEMP%\godot60.json
```

## 5. Achado: o JSON do Godot não é IEEE-exato

`JSON.stringify(v, "", true, true)` grava `0.0002886172890222971` como
`0.00028861728902229` (perde bits em `|x| < 1`) e o parser do Godot devolve um
double que só fecha com o próprio escritor — o `nlohmann` (arredondamento
correto) lê o **mesmo texto** e obtém outro double. Medido:

```
JSON.stringify({"x":0.0002886172890222971}) -> {"x":0.00028861728902229}
```

Por isso **snapshot nunca passa pelo JSON do Godot**: a exportação e o replay
usam `demo_to_snapshot_file`/`demo_from_snapshot_file`, que serializam e leem
com o `nlohmann` do núcleo. Antes disso, um replay de 5 dias divergia do CLI em
~1e-16 relativo (5 campos de `definitions` e depois o mundo). O save/load do
jogo não é afetado: ele passa `Dictionary` em memória, sem texto.

## 6. Aceite (medido em 2026-11-17, host do A0)

Todos com a DLL **debug** (a mesma do golden A0), editor Godot fechado.

a) **Caminho Godot inalterado.** `tools/world_equivalence.ps1 -Rounds 1
   -Baseline docs/economy/refactor/baseline/A0_60_urban.json` → **shas iguais**
   (`save 3e3ebbbe…1a3d0b7`, `status d79bfe32…c95ac9b`, money 2.200.539.587,40,
   0 falências). Bateria verde: `economy_integration_test`,
   `economy_ai_test`, `population_demo_test`, `economy_save_load_roundtrip_test`
   (60 d, save no dia 30 → **0 diffs**). Threads 1 × 8: mesmos dois shas; o
   único campo diferente entre os dois dumps é `meta.threads` (o número pedido).
b) **CLI determinístico.** Duas execuções de 60 d → **mesmo SHA-256**
   (`79c7897a415c6c593230c64a2adda908bf97046d0dcfdccc50f253ccacbf8feb`).
c) **CLI = Godot sem IA.** 60 d do mesmo snapshot: replay do Godot e CLI
   produzem arquivos **byte-idênticos** (mesmo SHA-256 acima) e o comparador dá
   **0 diferenças com tolerância 0**, `definitions` incluído. 834 cidades, 0
   violações de invariante.
d) **Perf release sem regressão.** `tools/bench_canonical.ps1` (60 d × 3
   rodadas, editor fechado, DLL release de medição): tick de parede **35,98 ms**
   contra **36,58 ms** do A0 (**−1,63%**) e dia completo **153,82 ms** contra
   **155,51 ms** (**−1,09%**); variação entre rodadas 1,58% (limite 3%). As
   fases ficam no ruído (`status` 15,51 vs 15,86; `trade` 6,33 vs 6,27;
   `arrival` 5,40 vs 5,48). A DLL debug foi restaurada no slot com hash
   conferido pelo próprio script.

Números de tempo (debug, informativos): snapshot 54.875.755 B (834 cidades, 846
construções, 113 produtos); replay do Godot 60 d em 4,36 s; CLI 60 d em
12,5–12,7 s, dos quais **~9,8 s são a carga** do snapshot (parse de 52 MiB +
montagem do mundo) e ~47 ms por dia de tick.

## 7. Limites desta fase (o que NÃO existe ainda)

- **Sem IA.** A IA econômica é GDScript até a **A5**; o CLI roda só o tick. O
  comparador CLI×Godot usa `--ai=0` por construção.
- **Sem bootstrap.** `demo_seed_industry` vive na fachada (chama `demo_build`);
  o snapshot precisa vir com `industry_seeded = true` e o CLI **recusa** um
  snapshot sem ele em vez de fingir um mundo diferente.
- **Sem modos `legacy`/`benchmark` removidos e sem renomear `demo_*`** — fica
  para a A1b, junto com a limpeza do nome das APIs.
- **Carga do snapshot é o gargalo do CLI** (~10 s para 52 MiB): o parse JSON e a
  construção do `Value` não são caminho quente do jogo, então ficaram fora desta
  fatia; a A2 (IDs inteiros e arrays contíguos) é onde o custo de carregar/rodar
  em escala será atacado.
- **Memória no save do jogo:** `demo_to_dict` agora materializa a árvore
  `Value` e converte para `Dictionary` (pico ~2× o dicionário). O caminho do
  tick não muda; o save contínuo é uma melhoria da A1b/A2.
- `economy_panel_test.gd` não foi rodado (falha pré-existente por BOM em
  `core/ui/city_economy_panel.gd`).
- **Configuração do build importa na comparação.** O CLI e a DLL têm de ser da
  mesma configuração (debug × debug ou release × release), como no A0: não há
  `fast-math`, mas o `-O2` do release pode contrair `a*b+c` em FMA e mudar o
  último bit. `gdextension/bin/opengs_sim_cli.exe` é da última configuração
  construída (as duas escrevem no mesmo `bin/`).
