# H1 — Modularização da fachada (log dos passos)

Plano: `H_modularizacao_escala_precos.md` §4. Regra de ouro: **refatoração pura antes de
qualquer número mudar**. Cada passo é um commit próprio, com build e portão de bit-identidade;
qualquer diferença de hash = passo revertido, não aceito com tolerância.

## H1a — `edge/definitions_loader` (carregador de definições)

**Escopo:** mover para `gdextension/src/edge/definitions_loader.{h,cpp}` os cinco métodos da
fachada que carregam dado bruto no `WorldState`. Os corpos foram movidos **verbatim** (mesmas
linhas, mesma ordem de iteração, mesmos nomes de campo); só o TU de definição mudou.

| Método | Linhas em `economy_sim.cpp` (antes) | Linhas movidas |
| --- | --- | --- |
| `EconomySim::set_currencies` | 936-1074 | 139 |
| `EconomySim::set_goods` | 1077-1173 | 97 |
| `EconomySim::set_recipes` | 4944-5060 | 117 |
| `EconomySim::seed_industry` | 5062-5298 | 237 |
| `EconomySim::set_needs` | 5300-5521 | 222 |
| **Total** | | **812** |

**Antes/depois:**

- `gdextension/src/economy_sim.cpp`: **12.497 → 11.680 linhas** (−817: 812 de corpo + 5 linhas em
  branco separadoras). Nada mais mudou no arquivo: `git diff` é só remoção.
- `gdextension/src/edge/definitions_loader.cpp`: **845 linhas** (31 de preâmbulo + 812 movidas + 2
  de fecho).
- `gdextension/src/edge/definitions_loader.h`: âncora do módulo (documentação; as declarações
  continuam em `economy_sim.h`, porque os métodos são de `EconomySim`).
- `gdextension/CMakeLists.txt`: `src/edge/definitions_loader.cpp` no alvo `economy_gdextension`.

**Por que continuam métodos de `EconomySim`:** os cinco escrevem direto em `ws_`/`sim_core_` sob o
`mutex_` recursivo da fachada; extrair para funções livres exigiria abrir membros privados e
mexer em contrato — o plano H1a pede só a mudança de arquivo.

**Dependências:** o único helper do namespace anônimo de `economy_sim.cpp` usado pelos corpos
movidos é `safe_nonneg` (8 usos). Foi duplicado em `definitions_loader.cpp` com o mesmo corpo
(inline, namespace anônimo): mesma semântica, mesmo resultado. `economy_sim.cpp` mantém o seu
(usado por `parse_region` e outros). Os lambdas `recipe_for`/`seed_final_chain` são locais de
`seed_industry` e viajaram junto.

**Nota de encoding:** os comentários de `economy_sim.cpp` já estavam com mojibake UTF-8
pré-existente (não introduzido aqui). A extração preservou os bytes: `definitions_loader.cpp`
carrega o mesmo texto dos comentários, sem reescrita.

**Harness:** `tools/c1_world_ai_365.gd:1813` — `"..." + "..." % [...]` formatava só o segundo
trecho (`%` liga mais que `+`), então o primeiro saía com `%d` literais. Passou a
`("..." + "...") % [...]`; mudança de harness, não de simulação.

## Portão de bit-identidade (H1a)

Build: DLL debug `cmake --build gdextension/build-debug -j2` (clang 23.1.5, godot-cpp 4.7) e
`sim_unit_tests` (`build-unit-tests`, `-j2`).

| Checagem | Resultado |
| --- | --- |
| `sim_unit_tests` | **62/62** casos, 451 asserções, exit 0 |
| 365 d **ON** (`c1_world_ai_365 --days=365`, cérebro ligado) | `trajectory_sha256` **67d950b7…** = baseline G1/D-131 (falências 0, violações 0, comida 93/179, drift 0,013175) |
| 365 d **OFF** (`OGS_C1_AI_BRAIN=0`) | `trajectory_sha256` **49c797ef…** = baseline D-126 (falências 0, violações 0, comida 96/179, drift 0,016524) |
| `tools/run_tests.ps1 -Tier pr -Base main` | ver seção "Bateria" abaixo |

Artefatos: `G:\opengs_tmp\h1_wt_365_on.json` e `h1_wt_365_off.json` (fora do repo).

## Bateria

`powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Tier pr -Base main`
(seleção por diff: `gdextension/src/**` → conjunto amplo de núcleo + sentinelas `pr`).

- **42/42 OK**, 0 falha, 0 crash, 0 timeout, 0 pulado — run `20261125_201647`
  (`G:\opengs_tmp\opengs_test_run_20261125_201647\summary.json`), 20:16→21:28.
- Inclui `sim_unit_tests` (CLI, 0,28 s), a rodada c1 de **90 d** (`c1_world_ai_90`, 225,65 s) e
  `determinism_cross_process` (65,8 s).
- O runner ficou ~45 min na fila do lock global porque outro agente (worktree `opengs-wt-g1b`)
  usava o lock; sem conflito, só espera.

## Próximos passos

H1b (`edge/status_city.cpp`, `fill_city_status`), depois H1d, H1c, H1e, H1f, H1g — na ordem do
plano, cada um com o mesmo portão. Sem merge até o QA fechar a série.
