# Protocolo da equipe de agentes — OpenGS Game

Documento vinculante para os agentes de `.opencode/agents/`. Se o protocolo e a preferência
pessoal de um agente divergirem, o protocolo vence.

- **Quadro compartilhado:** `docs/team/QUADRO.md`
- **Pareceres de revisão:** `docs/team/reviews/`
- **Regras do projeto:** `AGENTS.md` e `GAME_STATE.md` (este protocolo resume; o `AGENTS.md` manda)

## 1. Hierarquia

```text
Claude Opus — cabeça geral (fora do OpenCode; decide rumo, recebe resumos)
└── 3 GERENTES  [DeepSeek V4 Pro, mode all, 250 steps]
    ├── gerente-simulacao  — núcleo econômico C++/GDScript (gdextension/, core/economy/)
    ├── gerente-dados      — dados reais e pesquisa (data/definitions/, tools/build_*.py, tools/data/)
    └── gerente-qualidade  — testes, equivalência, performance, auditoria; pode vetar commit
        └── EXECUTORES / VERIFICADORES  [DeepSeek Flash, mode subagent]
            ├── executor-flash    — executa uma parte definida; edita e roda comandos
            └── verificador-flash — confere o diff de outro executor; não edita nada
```

Especialistas pré-existentes ficam disponíveis aos gerentes quando o trabalho pedir julgamento
de área: `game-systems-designer`, `godot-engineer`, `ui-ux-designer`, `world-3d-engineer`,
`qa-docs`, `senior-architect`.

**Regras de invocação**

- Gerente pode invocar `executor-flash`, `verificador-flash` e os especialistas que fizerem sentido.
- `executor-flash` e `verificador-flash` **não invocam ninguém** (permissão `subagent: deny *`).
- O verificador de uma entrega é sempre **um Flash diferente** do que executou.
- O gerente é o único que commita.

## 2. Quadro compartilhado (`docs/team/QUADRO.md`)

Cada gerente tem uma seção. A tarefa entra antes do trabalho começar e é atualizada a cada passo.
Campos obrigatórios por tarefa:

| Campo | Conteúdo |
| --- | --- |
| `ID` | `S-001` (simulação), `D-001` (dados), `Q-001` (qualidade) |
| `Objetivo` | uma frase, resultado observável |
| `Critério de aceite` | numérico/verificável, nunca "melhorar" |
| `Subtarefas` | lista numerada, cada uma com responsável (`executor-flash`, especialista, gerente) |
| `Status` | `planejada` → `em execução` → `verificando` → `revisão` → `aprovada` / `bloqueada` / `vetada` |
| `Bloqueios` | o que impede e de quem se espera o quê |
| `Decisões` | decisão tomada + evidência (e link para `GAME_STATE.md` §14 quando for decisão de projeto) |

**Comunicação entre gerentes é assíncrona**, por seções `@gerente-x` no fim do quadro:

```md
### @gerente-simulacao  ← mensagem de: @gerente-dados · 2026-11-15
S-012 precisa de `data/definitions/economy/wages.json` com a coluna `unit`.
Bloqueio: o dado está em salário mensal; quem converte para diário?
```

Não há espera em tempo real: quem chega lê o que está escrito. Mensagem sem pergunta objetiva e sem
dono não gera ação.

## 3. Fluxo de trabalho (gate a gate)

```text
gerente planeja → executor-flash faz → verificador-flash confere (outro Flash)
→ gerente valida → gerente-qualidade revisa (núcleo/perf/dado/contrato) → gerente commita
```

1. **Planejar** (gerente dono): ler `GAME_STATE.md` e o código, escrever a tarefa no quadro.
2. **Fatiar**: partes pequenas, cada uma com arquivos permitidos, critério de aceite e comando de
   verificação. Parte sem verificação possível não está pronta para delegar.
3. **Executar** (`executor-flash`): um briefing por vez; paralelize só o que não compartilha arquivo.
4. **Verificar** (`verificador-flash`): diff contra critério, parecer `APROVADO`/`REPROVADO` com
   arquivo:linha e saída de comando. Sem evidência, é `REPROVADO`.
5. **Consolidar** (gerente dono): ler o diff, decidir, registrar decisão; duas reprovações no mesmo
   item = o briefing está ruim, refaça o plano.
6. **Revisar** (`gerente-qualidade`): obrigatório para C++, contrato de status, save/load, IA e
   caminho de performance. Veto bloqueia o commit até resolução ou decisão do dono do jogo.
7. **Commitar** (gerente dono): regras da seção 6.
8. **Relatar**: `RESUMO PARA O USUÁRIO` no quadro e na resposta.

## 4. Briefing para os Flash

```text
Objetivo:            (uma frase, resultado observável)
Contexto:            (arquivos específicos a ler; não peça leitura do projeto inteiro)
Arquivos permitidos: (lista explícita)
Arquivos proibidos:  (lista explícita)
Critério de aceite:  (checável pelo verificador)
Verificação:         (comando exato + saída esperada)
Não faça:            (refatorar, formatar, renomear, commitar)
```

Briefing sem critério de aceite é briefing inválido.

## 5. Regras do projeto (valem para todos os agentes)

### Cena e verificação

- **Nunca abra a cena principal** (`game/`) para "ver o jogo". Verificação é headless, teste ou captura.
- Testes que dependem do autoload `Economy` rodam a partir de uma **cena `.tscn`**, não por `--script` puro.
- `ui_capture` e `map_capture` rodam **com janela** (sem `--headless`), e a UI se confere em
  `layout_report.json` (quatro resoluções).
- Rode **apenas o teste relevante** à mudança (`AGENTS.md` §Testes). `economy_long_run_test.gd` é
  lento: só para mudanças de longo prazo. `economy_panel_test.gd` falha por **BOM pré-existente** em
  `core/ui/city_economy_panel.gd` — não é regressão sua.

### Processos

- Só encerre **PIDs de teste travados há mais de 10 minutos**, identificando o processo antes.
- **Nunca** mate o editor do Godot nem o processo/servidor do `godot-editor-mcp`.

### C++

- CMake: `G:/tools/cpp-tools/Lib/site-packages/cmake/data/bin/cmake.exe`
  (`-S gdextension -B gdextension/build-debug`, depois `--build`).
- Build sem a **DLL debug real** em `gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll`
  não conta como feito. Se um experimento trocou a DLL, restaure a `.real_debug` no fim.
- Não edite o núcleo C++ sem build + teste de integração.

### Dados

- Fonte estatística primeiro; fato histórico documentado só como último recurso.
- **Nunca inventar** número, cidade, nação ou estado de simulação. Sem dado, ausência explícita.
- Toda série carrega citação (fonte, ano-base, unidade) e passa por validador.

### Idioma e contexto

- Conversa em pt-BR; identificadores de código em inglês; i18n pt-BR/en/es preservado.
- Contexto é o maior custo: briefings curtos, sessão nova por tarefa, sem despejar arquivos inteiros.

## 6. Commit (somente o gerente dono)

- `git add` **explícito, arquivo por arquivo**. Nunca `git add -A` nem `git add .`.
- **Nunca** inclua no commit:
  - `docs/economy/balance_review_sim*.json`
  - `docs/economy_ai/decisions.json`
  - `docs/economy_long_run/*`
  - estudos de `tools/icon_forge/` (previews e JSONs de estudo; `resource_icons.json` e os SVGs só
    quando forem o trabalho)
  - `Claude outputs/*`
  - `tools/_tmp_*`, `tools/_debug_*`
- Mensagem gravada em arquivo **UTF-8 sem BOM** e aplicada com `git commit -F <arquivo>`.
- As duas últimas linhas da mensagem são **exatamente**:

```text
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_015K1t1fgVfBDkWEVWjX7dpM
```

- Nada de `git reset`, `git clean`, `git checkout --`, `git restore`, `git push` sem autorização.
  O worktree pode ter trabalho não commitado do usuário — não sobrescreva.
- Mudança de sistema/mecânica/dado/arquitetura atualiza as seções afetadas do `GAME_STATE.md` e
  registra decisão nova em §14, **no mesmo commit**.

## 7. RESUMO PARA O USUÁRIO

Todo gerente encerra com um bloco pronto para repasse (é o que a cabeça geral lê):

```text
RESUMO PARA O USUÁRIO:
- O que mudou (1 linha)
- Números/evidência (1-2 linhas)
- Testes rodados e resultado (1 linha)
- Commit/hash (1 linha)
- Pendências e riscos (1-2 linhas)
```

Máximo 8 linhas. Sem jargão desnecessário. Sem promessa que não foi verificada.
