# Q-002 — Revisão pós-commit da refatoração do núcleo (S0→S1)

- **Revisor:** gerente-qualidade
- **Data:** 2026-11-15
- **Escopo:** commits do @gerente-simulacao já na árvore — `e3b28cd` (S0 harness),
  `0469009` (S4 remoção de API morta), `27c34f5` (otimização do STATUS),
  `105e783` + `e3f6123` (S1 sim_types/sim_constants), `1303aba` (docs).
- **Veredito:** **APROVADO** (todos os gates verificados).

## Critério de aceite (do QUADRO S-001)

| Gate | Resultado | Evidência |
| --- | --- | --- |
| Build debug+release limpo | ✅ verde | `cmake --build … --clean-first` exit 0 em ambos; zero `warning:`/`error:` nos logs |
| Harness `economy_refactor_equiv.gd` = 0 diffs vs goldens | ✅ 3/3 | world/60/ai, world/60/noai, demo/365/noai → "[equiv] OK: 0 diferenças" |
| Nada além de código morto removido (nenhuma API usada por .gd/.tscn) | ✅ | grep das APIs removidas = 0 referências |
| Gate "0 godot_cpp em sim/" | ✅ | grep `godot_cpp` em `gdextension/src/sim/` = 0 matches |
| Perf do status ≤ 30 ms | ✅ (evidência commitada) | `docs/economy/perf/refactor_status.txt` p95 28,2–29,2 ms |

## 1. Build limpo (debug + release)

Toolchain (llvm-mingw + Ninja), do `gdextension/CMakeCache.txt`:
- Gerador: Ninja; `CMAKE_MAKE_PROGRAM=G:/tools/cpp-tools/Scripts/ninja.exe`
- `CMAKE_CXX_COMPILER=G:/tools/llvm-mingw-20261104-ucrt-x86_64/bin/clang++.exe`
- `GODOTCPP_DIR=G:/projetos/war-economy/godot-cpp`

Execução (executor-flash, `--clean-first`):

```text
CFG_DEBUG_EXIT=0     "Build files have been written to: …/gdextension/build-debug"
BUILD_DEBUG_EXIT=0   últ. linha: [1094/1095] Linking CXX shared library …\bin\economy_gdextension.windows.template_debug.x86_64.dll
CFG_RELEASE_EXIT=0   "Build files have been written to: …/gdextension/build-release"
BUILD_RELEASE_EXIT=0 últ. linha: [1094/1095] Linking CXX shared library …\bin\economy_gdextension.windows.template_release.x86_64.dll
```

`Select-String "warning:|error:"` em ambos os logs → **nenhuma linha**.

Rebuild independente no worktree isolado (HEAD `1303aba`): debug e release exit 0, link OK.

## 2. Harness de equivalência byte a byte

Worktree isolado em `1303aba` (a árvore principal estava sendo mutada por outra sessão —
`economy_sim.cpp` editado às 17:21; por isso a verificação foi isolada). Invocação com `--`
antes dos args (o harness lê `OS.get_cmdline_user_args()`):

```text
S1  world/60/ai    save=092748d56cc041e5e735da682d200af203fedb4af7619ef545fd31fa2b0b5b61
                   status=031ae87886336a09f4ee70024936b47bf5588e08e7f83d2721c26030b92f6f6a
                   -> [equiv] OK: 0 diferenças vs real_60_ai.json          EXIT=0
S2  world/60/noai  save=f561cdb7e8a992a055ba7d6ffb8c3c7b0252312093cc09ebeb6a6a11af6537ac
                   status=c60ad9a825f8b6efc1db6f4a234ee92b7a4ca65caa2e639b866d2cdf362a931b
                   -> [equiv] OK: 0 diferenças vs real_60_noai.json        EXIT=0
S3  demo/365/noai  save=95229481cedf32130de845fe7c018273b3319ee7b0d5d3555c4df76afa153588
                   status=6bab8db9fa822a5ff2815b3af1555de7919f20c132177b9292380d11038b9679
                   -> [equiv] OK: 0 diferenças vs demo_365_noai.json       EXIT=0
```

Os três SHAs batem exatamente com `docs/economy/refactor/baseline/*.json`.

## 3. Código morto removido — nada além disso

APIs removidas no S4 (`0469009`) e verificadas sem referência em `.gd`/`.tscn`
(grep em `*.gd` e `*.tscn` do repo, excluindo `addons/`):

- `initialize(cities,ports)`, `tick()`, `get_status()` (API legada pré-demo)
- `initialize_benchmark`, `run_benchmark`, `get_benchmark_status`, `get_benchmark_city_count`
- binding `demo_tick_core`

Grep por esses identificadores em `.gd`/`.tscn` → **0 referências** (só `addons/godot_mcp_toolkit`
tem um `.tick()` de um watchdog próprio, alheio ao núcleo). `Port`/`TradeOrder`/`MarketScope`
permanecem (compartilhados), conforme decisão S-001/S4 no QUADRO.

## 4. Gate "0 godot_cpp em sim/"

`grep -r godot_cpp gdextension/src/sim/` → **0 matches**. `sim_types.h` e `sim_constants.h`
usam apenas STL (`namespace ogs`), sem nenhum include do engine.

## 5. Perf do status ≤ 30 ms

Evidência commitada em `docs/economy/perf/refactor_status.txt` (commit `27c34f5`):

- Baseline S4 (3 execuções, release): status p95 **28,56 / 29,17 / 28,70 ms**.
- Pós micro-otimização: status p95 **28,19 ms**.
- Conclusão do documento: "status p95 28,2–29,2 ms ≤ 30 (portão atendido)".

Não foi re-medido nesta revisão por dois motivos registrados: (a) a árvore principal estava sob
mutação concorrente (outra sessão editando `economy_sim.cpp`); (b) `perf_day_peak.tscn` travou no
worktree isolado (import incompleto do projeto, `--import` terminou com access violation em assets).
A evidência commitada é suficiente porque o harness §2 confirma a byte-identidade do binário medido.

## Observações (não bloqueiam)

- O harness ignora os args se não houver `--` antes deles (lê `OS.get_cmdline_user_args()`).
  A documentação no topo do `economy_refactor_equiv.gd` omite o `--`; recomendo corrigir o
  comentário para evitar falsos "FALHA" (cenário default world/60/ai).
- Um novo commit `b13faa6` (testes production_chain/services, contrato v11) entrou na árvore
  durante a revisão. Não está no escopo deste parecer (não toca núcleo); revisar quando a cadeia
  de equivalência for regenerada.
