# Modo TURBO — ferramenta de teste (não é feature do jogador)

> Branch `dev/turbo-tests` (worktree `opengs-wt-turbo`), saída de
> `balance/global-resources` (`69d9cc2`). Decisão registrada em D-078 do
> `GAME_STATE.md`.
>
> **O que é:** um atalho de TESTE/AGENTE para rodar centenas de dias de economia
> em segundos, ignorando o passo de 1 dia = 15 s do relógio do jogo.
> **O que NÃO é:** feature do jogador. Não existe botão, tecla, atalho, entrada
> de HUD, texto de i18n nem opção de menu — nada em `ui/`, `core/ui/` ou `map/`
> conhece este modo. Sem a flag, o caminho do jogo é bit a bit o de sempre.

## 1. Como ligar

Só por linha de comando (depois de `--`) ou por variável de ambiente:

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"

# 60 dias o mais rápido possível; sai sozinho e imprime a medição
& $godot --path . -- --turbo --bench-days=60

# lote e cadência explícitos (12 dias por tarefa, publica a cada 12)
& $godot --path . -- --turbo --turbo-days-per-frame=12 --turbo-render-every=12 --bench-days=365

# modo normal na velocidade máxima (para comparar), medindo 5 dias
& $godot --path . -- --bench-days=5 --bench-speed=3
```

| flag | ambiente | efeito |
|---|---|---|
| `--turbo` | `OGS_TURBO=1` (`true/yes/on`) | liga o turbo; lote padrão 4 dias, publicação a cada lote |
| `--turbo-days-per-frame=N` | `OGS_TURBO_DAYS_PER_FRAME` | dias por tarefa da `WorkerThreadPool` (padrão 4, mínimo 1) |
| `--turbo-render-every=N` | `OGS_TURBO_RENDER_EVERY` | de quantos em quantos dias a UI/mapa/status são publicados (nunca menor que o lote) |
| `--bench-days=N` | `OGS_BENCH_DAYS` | mede a parede até o dia N, imprime `[bench] ...` e sai com 0 (**não** liga o turbo). O corte cai no primeiro dia **publicado** ≥ N, então `to_day` pode passar de N em até `render_every` dias |
| `--bench-speed=N` | `OGS_BENCH_SPEED` | força a velocidade 0..3 (3 = 15 s/dia, a máxima do jogo) |

A linha de comando vence o ambiente quando as duas fontes aparecem. Valor
numérico inválido mantém o padrão; a presença da flag ainda liga o modo.

## 2. O que muda (e o que não muda)

O relógio da economia (`core/economy/economy.gd`) é um acumulador de fração do
dia (`SPEEDS`), e o dia roda numa thread do pool (`tick_scheduler.gd`). Em turbo:

- **o relógio é ignorado**: `_process` não acumula `_day_fraction`; quando a
  tarefa anterior volta, a próxima é agendada no mesmo frame. A velocidade
  escolhida (inclusive a pausa) não tem efeito — é ferramenta, não controle;
- **o worker roda um lote de `days_per_frame` dias por tarefa**
  (`tick_scheduler.days_per_task`): N chamadas seguidas de
  `sim.tick_and_publish()` na MESMA tarefa. Continua havendo **um escritor só**
  e um `join` por tarefa;
- **a UI/mapa/status só são atualizados a cada lote** (e a cada
  `render_every` dias, se maior): os sinais `status_updated`/`trade_orders_ready`
  saem uma vez por publicação, então comboios, animações e o `refresh_modes` do
  mapa pulam os dias intermediários — é esperado ver o dia saltar de 4 em 4;
- **a ordem do dia no núcleo não muda nada**: a IA econômica continua rodando
  como fase do `tick_and_publish` (A5/D-076) e a fila de comandos (A3) continua
  aplicada na primeira coisa de `tick_core()`. Um comando emitido pela main
  thread com o dia em andamento entra no início do dia seguinte, exatamente como
  no jogo normal;
- sem a flag: `days_per_task = 1`, `_process` segue o acumulador e `_publish`
  não entra em nenhum bloco novo.

## 3. Determinismo (o que está provado)

O turbo **não** muda a simulação: ele só muda quem chama o tick e quantas vezes
por tarefa. Nenhum dia depende do relógio de parede, então N dias em turbo = N
dias no modo normal.

- `tools/turbo_equivalence_test.gd` — quatro mundos idênticos (cenário real, IA,
  camada urbana §19, geografia) com 60 dias:
  - A normal (`tick_day()` na main) × B turbo (lote 7): SHA-256 do `to_dict()`
    **e** do `get_status()` iguais;
  - B publica **9** resumos (ceil(60/7)), provando a publicação por lote;
  - C turbo lote 1 com `price_shock("wheat", 1.1)` depois do dia 30 × D normal
    com o mesmo comando no mesmo ponto: save, status **e log de comandos**
    iguais, com `apply_day == 30` no log e zero comandos pendentes.
- `tools/world_equivalence.ps1 -Days 60 -Rounds 2` — o harness canônico do
  refactor continua fechando **0 diferenças** com este branch (o mundo de 60 d
  é o golden da recal3: `save dcad384c…`, `status 4f18522d…`).

## 4. Medição (dias/segundo)

Build **debug** (DLL `template_debug`, sem perfil), host do projeto (Ryzen 7
5700X3D), 2026-11-17. Medido com `tools/turbo_bench.ps1`, que roda a **cena real
com mapa** e o **headless** nos dois modos e extrai a linha `[bench]` de cada
execução:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/turbo_bench.ps1 `
  -NormalDays 5 -TurboDays 120
```

| cena | modo | janela medida | dias/s | por dia |
|---|---|---|---|---|
| janela (mapa real) | normal (speed 3) | 1 → 5 | **0,066** | 15,09 s |
| janela (mapa real) | turbo (lote 4) | 4 → 120 | **4,40** | 227 ms |
| headless | normal (speed 3) | 1 → 5 | **0,066** | 15,04 s |
| headless | turbo (lote 4) | 4 → 120 | **4,17** | 240 ms |

**Turbo ≈ 66× o modo normal** na cena real (63× headless). O normal é limitado
pelo relógio, não pelo núcleo: 15 s por dia na velocidade máxima (`SPEEDS[3]`).
No turbo o limite passa a ser o custo do dia (~227 ms neste build **debug** com
IA + camada urbana; a IA roda dentro do dia, D-076). Não é número de release —
`docs/economy/perf/baseline_A0.json` mede o tick de parede em build release.

O tamanho do lote muda pouco (o teto é o custo do dia): 60 dias headless com
`--turbo-days-per-frame=1` dá **3,57** dias/s e com `=16`, **3,84** (janelas
curtas medem menos que as de 120 dias por causa do aquecimento). O padrão 4 é o
equilíbrio entre latência de publicação e overhead de tarefa.

## 5. Limites e cuidados

- **Não é atalho de jogador.** Nada publica o modo na UI; quem liga é a CLI ou o
  ambiente. Não adicione botão/tecla: o dono decidiu que não existe.
- **Comando intercalado dia a dia** (testes da fila A3 que dependem do dia
  exato) pede `--turbo-days-per-frame=1`: o turbo continua sem esperar o relógio,
  mas a fronteira de dia fica determinística. Com lote > 1 o comando emitido no
  meio do lote cai no início do próximo dia que o worker alcançar — o que é
  seguro (fila + mutex) mas não é um dia fixo.
- **Publicação esparsa**: com lote/render grandes a UI fica N dias atrás do
  mundo; `Economy.last_summary` e `game_date()` só andam quando publicam.
- **`--bench-days`** existe para automatizar a medição; ele encerra o jogo
  (`quit(0)`) no primeiro dia **publicado** ≥ N — então `to_day` pode passar de N
  em até `render_every` dias (é o dia que realmente existe no status). Com
  `--bench-speed=0` o relógio normal fica pausado e o bench nunca termina
  (escolha de quem roda).
- **Custo**: turbo usa um núcleo inteiro continuamente; não deixe rodando sem
  `--bench-days` se a máquina estiver medindo outra coisa.
- O turbo não substitui `tools/world_equivalence.ps1` como prova de
  equivalência: ele acelera o caminho do jogo, não valida o núcleo.
- O headless roda a **cena principal** (só sem janela): o mapa carrega os PNGs
  do atlas como antes e emite os mesmos avisos de import do jogo normal — não
  afetam a medição, que é do relógio da economia. As medições acima foram feitas
  com a máquina livre (o bench canônico de outra frente rodando na mesma máquina
  invalida os números).
