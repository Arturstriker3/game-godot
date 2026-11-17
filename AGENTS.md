# OpenGS Game — instruções para agentes

## Fonte de verdade

1. `GAME_STATE.md` — estado real e consolidado do jogo. Consulte antes de qualquer mudança importante.
2. Código, cenas e `project.godot` — a fonte última; em divergência, o código vence.
3. `DESIGN.md` — design system, HUD, mapa, câmera, componentes e i18n.
4. `docs/` — relatórios e evidências de UI, mapa e economia.

Nunca assuma que uma funcionalidade existe porque está documentada. Confirme no código.

## Regras do projeto

- Não invente números, cidades, nações ou estados de simulação. Onde não há dado, mostre um estado vazio honesto.
- Preserve i18n em pt-BR, en e es; não quebre layout ao trocar idioma.
- Não rode `git reset`, `git clean`, `git checkout --` ou `git restore` sem autorização.
- O worktree pode conter trabalho não commitado do usuário. Não sobrescreva.
- Mudanças pequenas, com escopo explícito e verificação antes de encerrar.
- MCPs de cena começam em modo somente leitura. Mutações exigem aprovação.
- Não edite o núcleo C++ da economia sem rodar build e testes.
- Modelos Claude (Sonnet/Opus) só com autorização explícita do usuário — sem orçamento por padrão; use DeepSeek.
- Ao concluir uma implementação que altere sistema, mecânica, dados ou arquitetura, atualize as seções afetadas do `GAME_STATE.md`.
- Não duplique sistemas existentes; leia o código antes de criar algo novo.
- Ícones de recurso NÃO se desenham à mão nem se baixam: edite `tools/icon_forge/resource_icons.json`
  seguindo `tools/icon_forge/README.md` e rode `check`/`build` (o Godot importa os SVGs em
  `assets/icons/resources/` automaticamente). Eles são arte própria do projeto.

## Comandos

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"

# Testes headless (runner SceneTree; falha retorna != 0)
& $godot --headless --path . --script res://tools/economy_integration_test.gd
& $godot --headless --path . --script res://tools/economy_ai_test.gd
& $godot --headless --path . --script res://tools/economy_long_run_test.gd
& $godot --headless --path . --script res://tools/economy_panel_test.gd
& $godot --headless --path . --script res://tools/population_demo_test.gd
& $godot --headless --path . --script res://tools/camera_limit_test.gd
& $godot --headless --path . --script res://tools/territory_test.gd

# Camadas do mapa atlas (requer pyshp e scipy)
python tools/build_atlas_layers.py

# Captura e auditoria de UI
& $godot --path . res://tools/ui_capture.tscn -- --phase=f4 --run=final --sizes=1920x1080,1600x900,1366x768,2560x1440

# Captura do mapa
& $godot --path . res://tools/map_capture.tscn -- --set=all --mode=political --tag=before --run=baseline_before
```

## Testes

Nem toda tarefa exige teste. Rode **apenas o relevante à mudança** para não atrasar a tarefa:

- Economia, C++, save/load, contrato de dados ou regras → teste headless de economia correspondente.
- Mudança visual de UI ou mapa → captura (`ui_capture` / `map_capture`) e `layout_report.json`.
- Documentação, configuração ou texto → **nenhum teste**.

Custo por tipo:

- Rápidos: `economy_integration_test.gd`, `economy_ai_test.gd`, `population_demo_test.gd`.
- Lento (rode só para mudanças de longo prazo): `economy_long_run_test.gd`.
- Falhando por BOM pré-existente em `core/ui/city_economy_panel.gd`: `economy_panel_test.gd` — não rode sem necessidade.

API do núcleo (A1b, D-069): `EconomySim` não tem mais o prefixo `demo_` — use
`initialize_world`, `tick_day`, `get_status`, `set_goods`, `build`, `invest`, `to_dict`…
O cenário AGRIA/PORTUS é fixture de teste (`initialize_scenario()`), não o jogo; por isso
`population_demo_test.gd` mantém "demo" no nome do arquivo. Chaves do save/status e o valor
`status.mode == "demo"` **não** mudaram. Detalhes em `docs/economy/refactor/A1b_cleanup.md`.

### Modo TURBO (só teste/agente, **não** é feature do jogador)

Acelera o relógio da economia sem o passo de 15 s/dia do jogo: o worker roda um
lote de dias por tarefa e o relógio é ignorado. Liga **só** por linha de comando
ou ambiente — não há botão, tecla, atalho, HUD nem texto de jogo. Sem a flag o
jogo roda exatamente como hoje.

```powershell
# 60 dias o mais rápido possível, saindo sozinho com a medição de dias/s
& $godot --path . -- --turbo --bench-days=60

# lote e cadência explícitos (8 dias por tarefa, publica a cada 8)
& $godot --path . -- --turbo --turbo-days-per-frame=8 --turbo-render-every=8 --bench-days=200

# equivalente por ambiente (para quem não controla os argumentos de usuário)
$env:OGS_TURBO = "1"; $env:OGS_BENCH_DAYS = "60"
```

- `--turbo` / `OGS_TURBO=1` liga; `--turbo-days-per-frame=N` (padrão 4) é o lote
  de dias por tarefa; `--turbo-render-every=N` é de quantos em quantos dias a
  UI/mapa/status são publicados (nunca menor que o lote).
- `--bench-days=N` mede a parede até o dia N e imprime
  `[bench] mode=... from_day=... to_day=... wall_s=... days_per_s=...` antes de
  sair — vale com e sem turbo (`--bench-speed=3` mede o normal na velocidade
  máxima). `OGS_BENCH_DAYS`/`OGS_BENCH_SPEED` fazem o mesmo.
- Provas de equivalência: `tools/turbo_equivalence_test.gd` (turbo × normal em
  60 d: mesmo save, mesmo status, mesmo log de comandos) e
  `tools/world_equivalence.ps1` (60 d, 2 rodadas).
- Medição normal × turbo (cena real e headless): `tools/turbo_bench.ps1`.
- Em turbo o relógio e a velocidade (inclusive a pausa) são ignorados e a UI só
  é atualizada a cada lote: comboios e animações pulam dias. Comando da fila A3
  emitido por um teste continua entrando no início do dia seguinte; para
  intercalar comando **dia a dia**, use `--turbo-days-per-frame=1`.
- Números, limites e decisão: `docs/economy/perf/turbo.md` (D-077 no `GAME_STATE.md`).

Ao encerrar, registre quais testes rodou, ou "nenhum" e o motivo.

## Mapa de arquivos

- `game/` — cena principal e fluxo de jogo.
- `core/` — economia, UI, urbano, i18n e áudio.
- `ui/` — componentes e telas do design system.
- `map/` — mapa, camadas e dados geográficos.
- `data/definitions/` — definições de dados.
- `assets/` — modelos, texturas, fontes e áudio.
- `tools/` — testes, capturas e scripts de apoio.
- `docs/` — relatórios e evidências.
- `addons/godot_mcp/` — add-on do MCP do editor.

## MCPs

- `godot-editor-mcp`: requer o editor Godot aberto e o add-on `godot_mcp` ativo. Por padrão só os toolsets `core` e `inspection` ficam habilitados. Para editar, peça autorização e chame `godot_enable_toolset("scene_edit")` (ou `scripts`, `runtime`, etc.) antes de usar a ferramenta.
- `blender`: requer o add-on `blender_mcp.py` ativo e o servidor iniciado no Blender. Validado com Blender 5.2.3, add-on 1.8 e protocolo 15.
- `sequential-thinking`: conectado; use para decompor problemas complexos e revisar o plano. Não substitui a leitura do código nem a verificação.
- `godot-mcp-toolkit`: plugin copiado para `addons/godot_mcp_toolkit`, mas o MCP está desabilitado no `opencode.jsonc`. Para ativar: habilitar o plugin em Project Settings > Plugins, confirmar a porta 6550–6560 no dock MCP, trocar `"disabled": true` por `false` e rodar `opencode reload`. Não mantenha dois stacks MCP de cena ativos sem necessidade.

## Modelos

Distribuição por agente (definida no campo `model` de cada `.opencode/agents/*.md`):

- `orchestrator` → DeepSeek V4 Pro (conversa principal/roteamento — barato).
- `game-systems-designer` → DeepSeek V4 Pro (economia/arquitetura/núcleo C++).
- `ui-ux-designer` → DeepSeek V4 Pro (design/HUD).
- `world-3d-engineer` → DeepSeek V4 Pro (3D/shaders).
- `godot-engineer` → DeepSeek V4 Pro (implementação de código).
- `qa-docs` → DeepSeek Flash (testes/docs baratos).
- `senior-architect` → DeepSeek V4 Pro (problemas difíceis; perguntar ao usuário antes de usar).

## Custo e contexto

O maior custo não é o número de agentes: é o **tamanho do contexto**. Cada mensagem
reenvia a conversa inteira.

- Comece uma **sessão nova por tarefa**; não deixe uma conversa crescer por horas.
- Consulte `GAME_STATE.md` em vez de reauditar o projeto.
- Não leia imagens/capturas sem necessidade (cada uma custa tokens).
- Peça relatórios **curtos** aos subagentes; não despeje arquivos inteiros.
- DeepSeek (Pro/Flash) para tudo; modelos Claude (Sonnet/Opus) só sob demanda explícita.
- Ao terminar, encerre a sessão em vez de continuar no mesmo contexto.
