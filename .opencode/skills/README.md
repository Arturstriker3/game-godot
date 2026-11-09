# Skills migradas do TRAE

Estas skills foram copiadas de `.trae/skills/` sem alterar as instruções originais. O TRAE permanece intacto.

## Skills do projeto

| ID | Quando usar |
|---|---|
| `game-ui-ux` | HUD, menus, overlays, fluxo de telas, foco e UI dirigida por estado. |
| `godot-ui` | Padrões de UI no Godot (panorama geral). |
| `godot-ui-control` | `Control`, containers, anchors, temas e foco no Godot 4.7. |
| `godot-gdscript` | GDScript idiomático, tipagem, ciclo de vida e sinais. |
| `godot-gdscript-headless-testing` | Testes com `godot --headless` e runner `SceneTree`. |
| `godot-nodes-scenes` | Composição de cenas, instanciação e autoloads. |
| `godot-resources` | `Resource`, `.tres`, `ResourceLoader` e dados tipados. |
| `godot-shaders` | Shaders `canvas_item` e `spatial`. |
| `godot-signals-groups` | Sinais, `Callable`, grupos e desacoplamento. |
| `godot-3d-essentials` | Cena 3D, câmera, luzes, ambiente e `GridMap`. |

## Skills globais

Copiadas para `~/.config/opencode/skills/` porque também eram globais no TRAE:

- `impeccable` — design/auditoria de interface e documentação de design system.
- `reverse-engineer-anything` — análise de artefatos e binários; o servidor MCP do REA não está configurado.

## Precedência para UI

1. `game-ui-ux` define o comportamento de produto, fluxo e estados.
2. `godot-ui` define padrões gerais de UI no Godot.
3. `godot-ui-control` define a API e a composição de nós `Control`.

Não duplique uma regra nas três: consulte a camada correspondente.

## Adaptações feitas

- `godot-ui`: removido o bloco `allowed_tools` do TRAE; adicionada uma nota sobre os nomes de ferramentas MCP no OpenCode.
- `impeccable`: referências internas de caminho passaram de `.trae/skills/impeccable` para `<skill-base-dir>`.
- Nenhuma instrução de conteúdo foi reescrita.
