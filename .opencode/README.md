# OpenCode no OpenGS Game

Guia rápido. A evidência completa está em `docs/opencode/MIGRATION.md`.

## Agentes

- `orchestrator` — padrão para novas sessões; planeja, delega e valida.
- `godot-engineer` — GDScript, cenas, nós, sinais e recursos.
- `ui-ux-designer` — HUD, design system, i18n e estados vazios.
- `world-3d-engineer` — mapa, modelos, shaders e Blender.
- `game-systems-designer` — economia, logística, C++ e dados.
- `qa-docs` — testes, capturas, auditoria e documentação.
- `senior-architect` — problemas difíceis; só quando solicitado explicitamente.

Peça uma tarefa normalmente; o Orchestrator escolhe o especialista. Para chamar um agente diretamente, use o seletor de agente ou `/agent <id>`.

## Modelos

- `orchestrator`, `godot-engineer`, `game-systems-designer`, `ui-ux-designer`, `world-3d-engineer`, `senior-architect` → DeepSeek V4 Pro.
- `qa-docs` → DeepSeek Flash (rotina).
- Modelos Claude (Sonnet/Opus) só com autorização explícita do usuário — sem orçamento por padrão.

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
