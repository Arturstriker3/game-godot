# Q-015 - sweep 365 d em PARALELO (um processo Godot por variacao, threads=1).
#
# O sweep padrao (tools/economy_world_sweep.gd) roda as variacoes em serie.
# Para a revisao Q-015 elas rodaram como processos simultaneos, com 1 worker
# por processo (S19 ligado por --urban=1). Cada processo grava o seu relatorio
# em user://world_sweep/; depois tools/aggregate_s19_sweep.py monta o baseline.
#
# Uso:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1
#   ... -Variants real,float -OutPrefix main_a7_
#   (-NoUrban acrescenta a variante real SEM --urban, o A/B que isola o S19)
#
# -Variants (padrao `real,float`): `real` e a variante peg de aceite e `float`
# e a SONDA de sensibilidade do cambio (`--fx=float`), cujo hash TEM de diferir.
# As 5 variacoes peg antigas (real, real_seed1, real_seed2, half, xgs) fecham o
# MESMO trajectory_hash — rodar todas e CPU a toa; qualquer subconjunto pode ser
# pedido por nome (os nomes historicos continuam aceitos).
#
# Saida: logs em %TEMP%\opengs_q015_sweep\*, relatorios em user://world_sweep/.

param(
  [int]$Days = 365,
  [switch]$NoUrban,
  [string]$OutPrefix = "s19_",
  [string]$Variants = "real,float",
  # P4 (recal3): baseline do portão de comida/falências. O portão compara a
  # MÉDIA DE 30 DIAS quando o baseline tem `food_avg30` (baseline agregado por
  # tools/aggregate_s19_sweep.py depois do recal3); senão cai para o último dia.
  [string]$Baseline = "res://docs/economy/perf/recal2_world_sweep.json",
  # Isentos declarados pelo head (o runner registra a queda em vez de reprovar).
  [string]$FoodExempt = "SOL,DJI,SAH,JOR,YEM"
)

$ErrorActionPreference = "Stop"
$project = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$godot = $null
$editor = Get-Process -Name "Godot_v4.7.2-stable_win64" -ErrorAction SilentlyContinue |
  Select-Object -First 1
if ($editor) { $godot = $editor.Path }
if (-not $godot -or -not (Test-Path $godot)) {
  $godot = (Get-ChildItem "$env:USERPROFILE\OneDrive" -Filter "Godot_v4.7.2-stable_win64.exe" `
    -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
}
if (-not $godot -or -not (Test-Path $godot)) { throw "godot nao encontrado" }

$logdir = Join-Path $env:TEMP "opengs_q015_sweep"
New-Item -ItemType Directory -Force -Path $logdir | Out-Null
# Q-016 (365 d, --urban=1, 5 processos) foi o baseline corrente ate a
# recalibracao de areas/populacao (`recal/data-areas-pop`): re-mediu-se o mundo
# DEPOIS de `data/province-area-fix` + `data/q016-city-pop` (baseline
# `recal_world_sweep.json`) e, na rodada 2, DEPOIS do porto `SGP-SIN`
# (`recal2_world_sweep.json`). `q16_world_sweep.json` e
# `construction_world_sweep.json` ficam historico. Na rodada 3 o baseline e
# `recal3base_world_sweep.json` (recal2 remesclado com a media de 30 dias,
# passado por -Baseline).
$baseline = $Baseline

$catalog = @(
  @{ name = "real"; extra = @("--fx=real") },
  @{ name = "real_seed1"; extra = @("--fx=real", "--seed=1") },
  @{ name = "real_seed2"; extra = @("--fx=real", "--seed=2") },
  @{ name = "half"; extra = @("--fx=half") },
  @{ name = "xgs"; extra = @("--currencies=0") },
  # SONDA (Q-016), nao variante de aceite: forca `regime: float` para provar que
  # o cambio ALCANCA a simulacao. As variantes peg TEM de fechar o mesmo hash; a
  # trajetoria desta TEM de diferir (ver tools/aggregate_s19_sweep.py).
  @{ name = "float"; extra = @("--fx=float") },
  @{ name = "float_probe"; extra = @("--fx=float") }
)
if ($NoUrban) {
  $catalog += @{ name = "real_nourban"; extra = @("--fx=real") }
}

# Selecao por -Variants (nomes separados por virgula); o padrao e `real,float`.
# ATENCAO: PowerShell nao distingue maiusculas em nome de variavel, entao o
# parametro `$Variants` NAO pode ser reusado — a lista roda em `$runs`.
$spec = $Variants
$runs = @()
foreach ($name in ($spec -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
  $match = $catalog | Where-Object { $_.name -eq $name } | Select-Object -First 1
  if (-not $match) {
    throw ("variante desconhecida: " + $name + " (aceitas: " +
      (($catalog | ForEach-Object { $_.name }) -join ",") + ")")
  }
  $runs += $match
}
if ($runs.Count -eq 0) { throw "-Variants vazio" }

$start = Get-Date
Write-Output ("[q015] inicio " + $start.ToString("HH:mm:ss") + " processos=" + $runs.Count)
$procs = @()
foreach ($v in $runs) {
  $urban = if ($v.name -eq "real_nourban") { "--urban=0" } else { "--urban=1" }
  $out = "user://world_sweep/$OutPrefix$($v.name)_$Days.json"
  $args = @("--headless", "--path", $project, "--script", "res://tools/economy_world_test.gd",
    "--", "--ai=1", "--days=$Days", $urban, "--threads=1",
    "--baseline=$baseline", "--out=$out", "--food-exempt=$FoodExempt") + $v.extra
  $log = Join-Path $logdir "$($v.name).log"
  $err = Join-Path $logdir "$($v.name).err.log"
  $p = Start-Process -FilePath $godot -ArgumentList $args -RedirectStandardOutput $log `
    -RedirectStandardError $err -WindowStyle Hidden -PassThru
  $procs += [pscustomobject]@{ name = $v.name; proc = $p; log = $log }
  Write-Output ("[q015] " + $v.name + " pid=" + $p.Id)
}
foreach ($item in $procs) { $item.proc.WaitForExit() }
$end = Get-Date
foreach ($item in $procs) {
  Write-Output ("[q015] " + $item.name + " exit=" + $item.proc.ExitCode)
  Get-Content $item.log | Select-String "dinheiro inicial|pior nacao|FALHAS|OK:" |
    ForEach-Object { Write-Output ("    " + $_.Line) }
}
Write-Output ("[q015] fim " + $end.ToString("HH:mm:ss") + " duracao_min=" +
  [math]::Round(($end - $start).TotalMinutes, 1))
