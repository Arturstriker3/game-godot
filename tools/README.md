# tools/ — scripts de teste, captura e apoio

Esta pasta é ferramenta de desenvolvimento; nada aqui é conteúdo do jogo. O
teste canônico é `tools/run_tests.ps1` (ver o cabeçalho do arquivo).

## Arquivos temporários de teste → `G:\opengs_tmp`

Regra do projeto: **nenhum artefato grande de teste no C:**. Runs, lock, logs,
summary e os JSON/CSV de sonda ficam no disco de temporários.

| O quê | Onde |
| --- | --- |
| Raiz dos temporários | `G:\opengs_tmp` (parâmetro `-TmpRoot`, sobrescrevível) |
| Runs (`opengs_test_run_*`) | `<TmpRoot>\opengs_test_run_YYYYMMDD_HHMMSS\` |
| Lock global | `<TmpRoot>\opengs_godot_test.lock` |
| Logs por teste / summary | dentro da pasta do run |
| `TEMP`/`TMP` de cada Godot filho | `<TmpRoot>\godot_tmp` |
| Artefatos dos scripts (`OPENGS_TMP`) | `G:\opengs_tmp` |

Como funciona:

- `tools/run_tests.ps1` usa `G:\opengs_tmp` por padrão. Se o disco/pasta não
  existir ou não aceitar escrita, cai para `$env:TEMP\opengs_test_tmp` com aviso
  no console — o teste continua rodando.
- Cada processo Godot filho recebe `TEMP`/`TMP` = `<TmpRoot>\godot_tmp` e
  `OPENGS_TMP` = `<TmpRoot>` no ambiente, então o motor e os scripts de teste não
  escrevem lixo no C:.
- A checagem `-MinFreeDiskGB` (padrão 10 GB) olha o disco do `TmpRoot`. O C: tem
  um piso de 2 GB que gera **aviso**, não aborto.
- Scripts que geram JSON/CSV grande leem `OPENGS_TMP` via
  `tools/tmp_paths.gd` (`TmpPaths.path(...)` / `TmpPaths.dir(...)`), com default
  `G:/opengs_tmp` e queda para o temp do SO. Hoje: `ai_scale_bench`,
  `world_scale_bench`, `economy_world_sweep`, `b5_infra_ab_365`,
  `price_trajectory_365`, `c1_world_ai_365`, `c2c_ab_365_test` e o log de
  evidência do `economy_ai_test` (este último só quando `OPENGS_TMP` está
  definido; o runner sempre define).

Para regravar um artefato versionado em `docs/` (ex.: `docs/economy/perf/*.json`),
rode a ferramenta direto com `--out=res://docs/...` (ou `OGS_*_OUT`) sem o
runner; dentro da bateria o destino é o temporário.

Trocar a raiz numa rodada:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -List economy_ai_test -TmpRoot "G:\opengs_tmp"
```
