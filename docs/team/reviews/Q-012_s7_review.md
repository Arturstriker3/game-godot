# Parecer Q-012 — S-017 (trade+arrival) + S7 (status_publish leve)

**Veredito: APROVADO**

- Escopo: Q-012 `c0a046f` (perfil fino), `ba24157` (arrival O(n)), `25b4abc` (caches de trade)
  + S7 inteiro `372ca42` (nations incremental), `abd7614` (`get_city_status` aditivo),
  `17d8bcc`/`d282f2b` (sinais leves + IA lê light), `f103149`/`1186aab` (tabelas planas
  buildings/deposits), `999afc4` (bloco leve final + fix da chave `services`),
  `76b6bad` (migração dos leitores), `42bc1df` (docs).
- DLL: debug real restaurada ao fim (SHA-256 `C518FE91…`, 20.024.320 B). Release medida
  com DLL `template_release` no slot `template_debug` (per Q-010).
- Ambiente: Windows, llvm-mingw (Clang 23.1.5), 16 núcleos, editor fechado, 0 processos
  Godot concorrentes durante o perf.

## 1. Auditoria do diff (núcleo C++ + IA)

Aprovada nos 5 itens do critério:

1. **Simulação intacta.** `save_sha256` dos 3 goldens **inalterado**: `real_60_ai`
   `991ee806…`, `real_60_noai` `e41dad60…`, `demo_365_noai` `c9449042…` — os mesmos desde
   o Q-009. Só o `status_sha256` mudou (formato do status): `7df720fb…`/`b6d6f7d8…`/
   `f447994a…`. Prova de que nenhuma regra econômica mudou.
2. **IA lê só `light` (snapshot), nunca `get_city_status` ao vivo.** `git grep get_city_status
   -- core/economy/ai/` = 0 ocorrências. Os caminhos por cidade usam
   `IndustryTemplates.bind_light`/`unbind_light` + `market_field`/`pop_field`/
   `buildings_of_type`, que leem os arrays planos do bloco `light` quando ligados; os
   fallbacks `city.get("markets"/"pop"/"deposits"/"buildings")` só são alcançados com
   status montado à mão (teste/ferramenta), nunca com o status diário leve.
3. **Bloco `light` correto vs `get_city_status`.** Guardas verdes (headless, DLL debug):
   `economy_light_signals_test.gd` → 94.242 mercados × 834 cidades, 834 incumbentes, 846
   construções e 0 jazidas conferidos **bit a bit**; `economy_city_status_test.gd` →
   detalhe sob demanda completo + bloco diário leve.
4. **Nenhuma tela/ferramenta lê os campos removidos da fonte diária.** `grep`
   `markets/pop/services/buildings/deposits` em `game/`, `core/`, `tools/`: `game/` tem **0
   referências**; os leitores de `core/ui/` (city_panel, hud, population_screen,
   economy_history) usam guarda `city.has("…")` (fixture) + fallback `get_city_status(id)`;
   a IA usa fallback documentado. Nenhum lê o bloco leve como se fosse pesado.
5. **Bug da chave `services` corrigido sem efeito colateral.** `DemoStatusKeys` ganhou
   `services("services")` (antes `StringName` vazio → bloco saía sob `""`); `k.services` é
   usado só em `economy_sim.cpp:2078` (publicação do bloco pesado). `DemoStatusKeys` **não
   é usado** em `edge/save_load.cpp` → sem impacto no save (`save_sha256` inalterado
   confirma).

## 2. Equivalência e bateria (executor-flash)

- **Goldens S0 3/3, threads=1 E threads=8, 6/6 OK ("0 diferenças")**:
  `real_60_ai` 991ee806/7df720fb, `real_60_noai` e41dad60/b6d6f7d8,
  `demo_365_noai` c9449042/f447994a — threads=1 ≡ threads=8 nos três.
- **10× `threads=8`** (`real_60_noai` sem baseline): **10/10 idênticos**
  (`e41dad60…`/`b6d6f7d8…`, money 2.200.539.587,40, 0 falências) — sem data race.
- **Round-trip real + demo: 2/2 exit 0** (`economy_save_load_roundtrip_test.gd`
  `--mode=world` e `--mode=demo`).
- **Bateria 8/8 exit 0**: integration, ai, population_demo, production_chain, investment,
  labor_market, pop_needs (via `--script`) + services (via cena `services_test.tscn`).
  Nenhuma linha `SCRIPT ERROR`/`FAIL`.
- **`economy_world_test` com e sem `--ai=1`: 2/2 exit 0** (drift 0,0000; pior nação BEL
  0,775 sem IA / EGY 0,748 com IA).
- **Sweep 365 d × 5 variações vs `services_fix_world_sweep_v2.json`: exit 0, bit a bit**
  — 14 falências de construção nas 5 variantes, 48.077 negócios / 13.495 falências de
  serviço, `DJI 0.394838716690075`, `SAH 0.220723431184531`, `SOL 0.986267218193767`;
  179/179 nações × 5 variantes com 0 diffs. O exit 0 é coerente: o baseline passado é o
  **pós-fix** (a comparação contra `f9_invest_world_sweep.json`, pré-fix, é que daria o
  exit 1 já decidido em Q-005/Q-007 — não é regressão nova).

## 3. Perf A/B (release tick + debug status)

**Tick release A/B ×3 intercalados** (`perf_day_peak --days=30 --tick-mode=worker
--threads=8`, DLL release no slot; p95 de "tick em regime (dia > 2)"):

| | run 1 | run 2 | run 3 | mediana |
|---|---|---|---|---|
| `128b8f8` (pré-S7, pesado) | 56,19 | 56,09 | 56,24 | **56,19 ms** |
| `42bc1df` (pós-S7, leve) | 43,10 | 42,90 | 43,05 | **43,05 ms** |

**Ganho: ~13 ms (−23%)** no tick p95 release — reproduz o "57→43 ms" do gerente.

**Status (fase) debug A/B** (`--profile=1`, `--tick-mode=sync --threads=8`, p95):

| fase | 128b8f8 | 42bc1df |
|---|---|---|
| status | 48,65 ms | **23,13 ms** (−52%) |
| trade | 38,14 ms | 38,06 ms (inalterado) |
| arrival | 12,45 ms | 12,30 ms (inalterado) |

A fase `status` (publicação Godot) caiu de ~48,65 → ~23,13 ms em debug — a alavanca real do
S7 (remoção dos blocos pesados `markets`/`pop`/`services`/`buildings`/`deposits` do status
diário). `trade`/`arrival` intactos (S7 não os tocou).

## 4. Conclusão e o que falta para ≤ 40 ms

- **Correção aprovada sem ressalva**: simulação intacta (save_sha256 inalterado), IA lê só
  `light` (nunca o estado vivo), light ≡ pesado bit a bit, nenhum leitor de campo removido,
  fix da chave `services` sem efeito colateral. Equivalência e bateria todas verdes.
- **Perf:** tick p95 release **43,05 ms** (era 56,19) — a meta **≤ 40 ms fica a ~3 ms**.
  A remoção dos blocos pesados cortou ~13 ms (release) / ~25,5 ms (debug, fase status).
  **O que falta para fechar os ~3 ms**: o custo restante da publicação leve por dia (os
  escalares + arrays do bloco `light` que mudam todo dia ainda cruzam a ponte GDExtension)
  e as fases seriais remanescentes (`trade`/`arrival` ~38/~12 ms em debug, bem menores em
  release sob -O2). Direção: S15 (SoA) / S17 (cadência) para trade/arrival — o paralelismo
  por cidade já foi avaliado e não é viável bit a bit (Q-011/Q-012, D-051).
- **Observação (não bloqueia):** o "erro da tela de Serviços" visto no `ui_capture` é do
  **harness** (`tools/ui_capture.gd:567` indexa `_hud.get("_screens")["services"]` sem
  checar, com `SHOW_DATA_SCREENS=false`), **não** da tela (`services_screen.gd`) nem do
  núcleo. É **pré-existente**: `services_screen.gd`, `ui_capture.gd`, `game/main_game.gd`
  e `EconomySim::demo_get_services_detail` são byte-idênticos entre `128b8f8` e `42bc1df`
  (o `hud.gd` só mudou num tooltip de jazidas, sem relação). Com `--screens=on`, as 5
  capturas de Serviços abrem sem um único erro. Recomendação operacional (do executor):
  capturar telas de dados com `--screens=on` enquanto `SHOW_DATA_SCREENS=false`.

DLL debug real restaurada (SHA-256 `C518FE91…`, 20.024.320 B). Nenhuma cena principal
aberta; editor não morto. Nenhum arquivo de código editado nesta revisão.
