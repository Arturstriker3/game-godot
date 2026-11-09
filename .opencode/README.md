# OpenCode no OpenGS Game

Guia rápido. A evidência completa está em `docs/opencode/MIGRATION.md`.

## Agentes

- `orchestrator` — padrão para novas sessões; planeja, delega e valida.
- `godot-engineer` — GDScript, cenas, nós, sinais e recursos.
- `ui-ux-designer` — HUD, design system, i18n e estados vazios.
- `world-3d-engineer` — mapa, modelos, shaders e Blender.
- `game-systems-designer` — economia, logística, C++ e dados.
- `qa-docs` — testes, capturas, auditoria e documentação.

Peça uma tarefa normalmente; o Orchestrator escolhe o especialista. Para chamar um agente diretamente, use o seletor de agente ou `/agent <id>`.

## Modelos

- `orchestrator`, `game-systems-designer`, `ui-ux-designer`, `world-3d-engineer` → Claude Sonnet 5.5 (arquitetura/design/visual).
- `godot-engineer` → DeepSeek V4 Pro (código).
- `qa-docs` → DeepSeek Flash (rotina).
- Claude Opus: somente com autorização explícita.

## MCPs

- `godot-editor-mcp` — conectado. Toolsets de escrita começam desabilitados; habilite apenas o necessário.
- `blender` — conectado.
- `sequential-thinking` — conectado; use para decompor problemas complexos.
- `godot-mcp-toolkit` — plugin copiado, MCP desabilitado; veja a ativação em `docs/opencode/MIGRATION.md`.

## Skills

- Projeto: `.opencode/skills/`.
- Globais: `~/.config/opencode/skills/`.
- Índice e precedência: `.opencode/skills/README.md`.

## Verificação

```powershell
opencode mcp list
opencode reload
```

Os comandos de teste e captura estão em `AGENTS.md`.
