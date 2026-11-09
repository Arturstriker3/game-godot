# Migração OpenCode — evidências

Data: 2026-11-09
Escopo: configurar o ambiente de desenvolvimento assistido por IA no OpenCode Desktop, sem alterar código, cenas ou dados de jogo.

## Arquivos criados

| Caminho | Função |
|---|---|
| `opencode.jsonc` | Configuração do projeto: agente padrão, MCPs e travas de segurança. |
| `AGENTS.md` | Regras compartilhadas, comandos, mapa de arquivos e política de modelos. |
| `.opencode/agents/*.md` | Seis agentes: Orchestrator, Godot Engineer, UI/UX, 3D World, Game Systems e QA & Docs. |
| `.opencode/skills/*/SKILL.md` | Dez skills do projeto migradas do TRAE. |
| `.opencode/skills/README.md` | Índice e precedência das skills. |
| `docs/opencode/MIGRATION.md` | Este relatório. |
| `addons/godot_mcp_toolkit/` | Plugin 1.1.2 copiado de `war-economy`; ainda não habilitado. |

Nenhum arquivo em `.trae/` foi alterado. Nenhum arquivo de jogo, cena ou asset foi modificado.

## Skills

- Dez skills do projeto copiadas para `.opencode/skills/`.
- Duas skills globais copiadas para `~/.config/opencode/skills/`: `impeccable` e `reverse-engineer-anything`.
- Comparação SHA-256: todos os arquivos são idênticos aos originais, exceto as adaptações intencionais:
  - `godot-ui/SKILL.md`: removido `allowed_tools` do TRAE; adicionada nota sobre nomes de ferramentas no OpenCode.
  - `impeccable`: 17 arquivos com `.trae/skills/impeccable` substituído por `<skill-base-dir>`.
- As 12 skills foram anunciadas pelo OpenCode; a skill `godot-gdscript-headless-testing` foi carregada em teste real.

## Agentes

| Agente | Modelo padrão | Escopo |
|---|---|---|
| `orchestrator` | `deepseek/deepseek-flash` | Coordena, delega e valida; não edita. |
| `godot-engineer` | `deepseek/deepseek-flash` | GDScript, cenas, sinais, recursos. |
| `ui-ux-designer` | `deepseek/deepseek-flash` | HUD, design system, i18n, estados vazios. |
| `world-3d-engineer` | `deepseek/deepseek-flash` | Mapa, modelos, shaders, Blender. |
| `game-systems-designer` | `anthropic/claude-sonnet-5-5` | Economia, logística, C++, dados. |
| `qa-docs` | `deepseek/deepseek-flash` | Testes, capturas, auditoria e docs. |

Testes de fumaça (sessões arquivadas, sem ferramentas):

| Teste | Resultado |
|---|---|
| `qa-docs` + DeepSeek | `QA OK.` |
| `game-systems-designer` + Sonnet | `SONNET OK.` |
| `orchestrator` + DeepSeek | `ORCHESTRATOR OK.` |
| `godot-engineer` | `GODOT OK.` |
| `ui-ux-designer` | `UI OK.` |
| `world-3d-engineer` | `3D OK.` |
| Delegação Orchestrator → QA | `DELEGATION OK.` |
| Carregamento de skill | `SKILL OK.` |

## Providers

- DeepSeek: credencial armazenada no OpenCode; modelos de catálogo `deepseek/deepseek-flash` e `deepseek/deepseek-v4-pro`.
- Anthropic: credencial armazenada; modelos `claude-sonnet-5-5`, `claude-opus-5-5`, `claude-haiku-5-5` e `claude-fable-5-1`.
- OpenRouter: **removido** a pedido do usuário (2026-11-09). O bloco `providers.openrouter` foi removido de `opencode.jsonc`.
- Nenhuma chave foi escrita no repositório.
- Claude Opus não foi testado e continua bloqueado por política: só com autorização explícita.

## MCPs

| MCP | Status | Evidência |
|---|---|---|
| `godot-editor-mcp` | **Conectado e validado** | Servidor 2026.11.04; bridge `ws://localhost:9080`; Godot 4.7.2-stable; projeto `G:/projetos/opengs-game/`; cena ativa `res://game/main_game.tscn`; árvore lida com `MainGame`, `WorldEnvironment`, `DirectionalLight3D`, `Player`. |
| `blender` | **Conectado e validado** | Pacote 2.2.7; Blender 5.2.3 LTS; add-on 1.8; protocolo 15; cena com `Cube`, `Light` e `Camera`. |
| `godot-mcp-toolkit` | **Configurado e desabilitado** | Servidor 1.1.2; plugin 1.1.2 copiado para `addons/godot_mcp_toolkit`; falta habilitar no editor e remover `disabled`. |
| `sequential-thinking` | **Conectado e validado** | Pacote `@modelcontextprotocol/server-sequential-thinking`; chamada de teste retornou `thoughtNumber: 1`, `nextThoughtNeeded: false`. |

### Ativação do toolkit

1. Godot → Project → Project Settings → Plugins → habilitar **Godot MCP Toolkit**.
2. Conferir no dock MCP a porta `6550–6560`.
3. Em `opencode.jsonc`, trocar `"disabled": true` por `false` no servidor `godot-mcp-toolkit`.
4. Rodar `opencode reload` e `opencode mcp list`.
5. Validar uma chamada de leitura antes de permitir mutações.

### Observação operacional

Havia duas instâncias antigas do `godot-editor-mcp`; a mais antiga ocupava a porta 9080 e o editor estava conectado a ela, enquanto a sessão nova não conseguia abrir o bridge. As instâncias antigas foram encerradas e o servidor foi recarregado. Se o bridge aparecer offline com o editor aberto, verifique processos duplicados e a porta 9080.

## Baseline de testes

Executado em 2026-11-09 com Godot 4.7.2-stable, sem alterar código:

| Teste | Resultado |
|---|---|
| `economy_integration_test.gd` | Passou (geografia, cadeia, investimento e save/load). |
| `economy_ai_test.gd` | Passou (trigo, farinha e ferro). |
| `population_demo_test.gd` | Passou (conservação e ciclo econômico). |
| `economy_long_run_test.gd` | Passou (365 dias sem violação de conservação). |
| `economy_panel_test.gd` | **Falhou por problema pré-existente**: BOM `U+FEFF` no início de `core/ui/city_economy_panel.gd`. |

O BOM foi encontrado apenas nesse arquivo. Nenhum código foi alterado para contornar a falha.

## Pendências

- Corrigir o BOM de `core/ui/city_economy_panel.gd` (uma alteração de código; exige autorização).
- Habilitar e validar o `godot-mcp-toolkit`, se ele for adotado.
- Rodar capturas de UI/mapa como baseline antes da primeira alteração visual.
- Opcional: instalar as skills recomendadas pelo `godot-editor-mcp` (`godot-getting-started`, `godot-playtest-and-debug`, `godot-expert`) e as Companion Skills do toolkit.
- Opcional: testar Claude Opus somente com autorização explícita.

## Rollback

- Remover `opencode.jsonc`, `AGENTS.md`, `.opencode/`, `docs/opencode/` e `addons/godot_mcp_toolkit/`.
- As configurações originais do TRAE permanecem intactas.
- O commit base é `6f2e45ead34849d141617d720f8c20cdeed54d3c`; existe a branch de backup `backup/pre-opencode-setup-2026-11-09`.
- O trabalho não commitado do usuário foi preservado.
