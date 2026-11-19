# Testes — protocolo da equipe

## Regra

**Toda tarefa roda testes SÓ por `tools/run_tests.ps1`.** Nenhum agente chama o Godot de teste
direto (nem `--headless --script`, nem cena, nem `Start-Process` manual). O runner é o dono do
lock, dos limites de memória/disco, do timeout e da limpeza de processos.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -All
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -List services_test,economy_integration_test
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Pattern "*economy*" -TimeoutSec 1200
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -List economy_long_run_test -TimeoutOverrides "economy_long_run_test=3600"
```

## O que o runner garante

- **Um Godot de teste por máquina.** Lock em `%TEMP%\opengs_godot_test.lock` (PID dentro) e
  checagem de processos `Godot --headless --path` de outras tarefas; quem chega depois espera.
  O lock é sempre liberado no `finally`. O editor Godot e o `godot-editor-mcp` nunca são tocados.
- **Piso de recursos antes de cada teste:** RAM livre ≥ 4 GB e `C:` ≥ 10 GB. Se estiver abaixo,
  espera até 5 min e aborta com relatório (código 4).
- **Cena × script automático:** `.gd` que `extends SceneTree`/`MainLoop` roda com `--script`;
  qualquer outro roda a `.tscn` correspondente (nunca `--script` numa cena). Script `Node`/`Node3D`
  sem `.tscn` é pulado com aviso. Tudo sempre com `--headless`.
- **Timeout por teste** (padrão 900 s, `-TimeoutOverrides "nome=segundos"`) e, no timeout,
  `taskkill /T /F` na árvore do Godot + `WerFault.exe` filho. Exit code é registrado;
  `0xC0000005` vira **CRASH** com o nome do teste.
- **Saída:** `summary.json` + `summary.txt` e um log por teste numa pasta própria em `%TEMP%`
  (`opengs_test_run_<data>`). Se tudo verde, os logs são apagados e só o resumo fica
  (`-KeepLogs` impede a limpeza).

## Seleção

- `-All` (ou sem seleção): todos os `tools/*_test.gd` + `tools/*_test.tscn` (sem duplicar).
- `-List a,b`: nomes com ou sem `_test`, `.gd`/`.tscn`.
- `-Pattern "*economy*"`: filtro por nome.

## Códigos de saída

| Código | Significado |
| --- | --- |
| 0 | tudo verde |
| 1 | pelo menos um FAIL / CRASH / TIMEOUT |
| 2 | erro de setup (Godot não achado, seleção inválida, nada para rodar) |
| 3 | espera pelo lock estourou `-LockWaitSec` |
| 4 | piso de recursos não atingido em `-ResourceWaitSec` |

## Diagnóstico

O `summary.json` guarda `exit_hex`, duração e as últimas linhas do log de cada teste vermelho.
Para investigar um crash, rode só o teste suspeito com `-List`; se precisar de mais contexto,
use `-KeepLogs` e leia os `.out.log`/`.err.log` no caminho impresso no resumo.

Se o processo do runner morrer no meio da bateria, a pasta da run fica **sem `summary.txt`/
`summary.json`** (o resumo só é escrito no fim) e o lock fica órfão em `%TEMP%\opengs_godot_test.lock`;
a execução seguinte remove o lock quando o PID não existe mais. Baterias longas devem ser
divididas com `-List`/`-Pattern` para não estourar o tempo da tarefa.

## Pendências conhecidas

- **Crash de acesso a memória nula (`0xC0000005`) sem teste identificado (19/11/2026).**
  A bateria de 79 testes selecionados rodou até o nono (todos verdes, `agri_calendar_owner_test`
  … `b7_informality_test`) e foi interrompida com o runner ainda dono do lock (PID 17568 morto);
  não há registro de `CRASH` nem de `TIMEOUT` nessa run. Os 16 eventos `0xc0000005` do Event Log
  do Windows são de 14:47, **antes** do runner: eram processos `Godot --headless --path` abertos
  em paralelo em outro worktree (`opengs-wt-g2`), alguns com o diálogo `ALERT!` aberto, mortos em
  bloco. Rodados de novo pelo runner, os suspeitos passam: `label_layout_test` (que antes travava
  900 s por falta de `--import`) em ~22 s, e as cenas de mapa/UI (`map_frame_test`, `map_pick_test`,
  `menu_smoke_test`, `nav_strait_test`, `minimap_*_test`, `nation_highlight_test`,
  `camera_limit_test`, `city_tooltip_test`, `cursor_hover_test`, `territory_test`) todas verdes.
  Quando o crash reaparecer, o nome sai no `summary.json` (`status: "CRASH"` + `exit_hex:
  0xC0000005`) e o log em `logs/<nome>.out.log`; reporte o teste antes de corrigir.
