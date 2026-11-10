# Ponte Claude Desktop → OpenCode/DeepSeek

Dá ao Claude Desktop "braços" que rodam **no seu computador**: ele decide e
delega; quem lê o código, edita, roda testes e usa os MCPs (Godot/Blender) é o
OpenCode com DeepSeek, dentro de `G:\projetos\opengs-game`.

Por que assim e não pela nuvem: o sandbox do claude.ai não enxerga seu disco e
não alcança `api.deepseek.com`. MCP local não passa por essa sandbox — o
servidor roda na sua máquina, na sua sessão, com as chaves já guardadas no
OpenCode (`opencode auth list` → Anthropic + DeepSeek `stored`).

## Arquivos

| arquivo | função |
|---|---|
| `server.mjs` | servidor MCP (stdio, sem dependências). Ferramentas: `deepseek_task`, `deepseek_result`, `opencode_agents`. |
| `selftest.mjs` | teste de fumaça. Sem flag: grátis (handshake + listagem). Com `--task`: uma tarefa real (~US$ 0,0002). |

O CLI do OpenCode é descoberto automaticamente em
`%APPDATA%\ai.opencode.desktop\cli\<versão>\opencode-cli.exe`; a tarefa roda
com `opencode-cli run --archived --auto --format json`, no diretório do projeto.

## Validação

```powershell
node tools/claude_bridge/selftest.mjs          # handshake, lista de ferramentas e agentes
node tools/claude_bridge/selftest.mjs --task   # inclui uma tarefa real no DeepSeek
```

Saída esperada no `--task`: `status: done`, agente `qa-docs`, ~4 s, custo ~US$ 0,0002.

## Instalação no Claude Desktop (Windows)

O Claude Desktop no Windows é empacotado como MSIX e **lê o config no caminho
virtualizado**, não no `%APPDATA%\Claude` que o botão "Edit Config" abre:

```
%LOCALAPPDATA%\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Roaming\Claude\claude_desktop_config.json
```

Esse arquivo já foi criado com:

```json
{
  "mcpServers": {
    "opengs-deepseek": {
      "command": "C:\\nvm4w\\nodejs\\node.exe",
      "args": ["G:\\projetos\\opengs-game\\tools\\claude_bridge\\server.mjs"],
      "env": { "OPENGS_PROJECT_DIR": "G:\\projetos\\opengs-game" }
    }
  }
}
```

Passos:

1. Claude Desktop → Configurações → **Developer** → habilitar (deixa a seção
   "Local MCP servers" visível).
2. Encerrar o app por completo pelo ícone da bandeja (não basta fechar a janela)
   e abrir de novo.
3. Confirmar que `opengs-deepseek` aparece como conectado.

Se continuar "No servers added" com o arquivo no lugar: a build da Microsoft
Store em algumas versões ignora MCP local. Instale a build de
`claude.ai/download` (também MSIX, mesmo caminho de config) e repita.

Logs de MCP: `%LOCALAPPDATA%\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Roaming\Claude\logs\`.

## Como usar

Peça normalmente; o Claude escolhe a ferramenta. Exemplos de instrução útil:

- "Delegue ao `godot-engineer`: rodar `tools/territory_test.gd` e corrigir se falhar. Me devolva só o resultado."
- "Peça ao `qa-docs` uma captura `ui_capture` em 1366x768 e um resumo do `layout_report.json`."
- "Antes de decidir, chame `opencode_agents` e escolha o especialista certo."

Campos de `deepseek_task`:

| campo | padrão | observação |
|---|---|---|
| `task` | — | obrigatório; escopo + critério de aceite |
| `agent` | `orchestrator` | ou `godot-engineer`, `qa-docs`, `game-systems-designer`, `ui-ux-designer`, `world-3d-engineer`, `senior-architect` |
| `model` | `deepseek/deepseek-flash` | `deepseek/deepseek-v4-pro` para trabalho pesado |
| `wait_seconds` | 30 | máximo 110; acima disso volta `status: running` |
| `timeout_seconds` | 900 | limite total em segundo plano |

Tarefa longa: a resposta vem `running` com um `task_id`; o Claude chama
`deepseek_result` com `wait_seconds` até concluir. O contexto do Claude recebe
só resumo, custo e tokens — o código fica aqui.

## Custos

- Claude: a tarefa e o resumo. Contexto curto por construção.
- DeepSeek: o trabalho (cada tarefa é uma sessão real; ~11k tokens de contexto
  base por chamada).
- Atenção: `game-systems-designer`, `ui-ux-designer`, `world-3d-engineer` e
  `senior-architect` estão configurados com **Claude via API paga**, não com sua
  assinatura. Para manter tudo em DeepSeek, passe `model: "deepseek/deepseek-v4-pro"`
  explicitamente.

## Segurança

- `--auto` aprova permissões não negadas, porque headless não tem como perguntar.
- As travas do `opencode.jsonc` continuam valendo: `git reset`, `git clean`,
  `git push`, `git checkout --`, `git restore` e edição de `.env*` seguem
  bloqueados ou pedindo confirmação.
- Tarefas destrutivas ainda merecem revisão: peça escopo fechado no `task`.
