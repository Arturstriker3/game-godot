# Q-015 - sweep 365 d em PARALELO (5 processos Godot, threads=1 cada).
#
# O sweep padrao (tools/economy_world_sweep.gd) roda as variacoes em serie.
# Para a revisao Q-015 elas rodaram como processos simultaneos, com 1 worker
# por processo (S19 ligado por --urban=1). Cada processo grava o seu relatorio
# em user://world_sweep/; depois tools/aggregate_s19_sweep.py monta o baseline.
#
# Uso:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1
#   (-NoUrban roda a variante real SEM --urban, o A/B que isola o S19)
#
# Saida: logs em %TEMP%\opengs_q015_sweep\*, relatorios em user://world_sweep/.

param(
  [int]$Days = 365,
  [switch]$NoUrban,
  [string]$OutPrefix = "s19_"
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
$baseline = "res://docs/economy/perf/construction_world_sweep.json"

$variants = @(
  @{ name = "real"; extra = @("--fx=real") },
  @{ name = "real_seed1"; extra = @("--fx=real", "--seed=1") },
  @{ name = "real_seed2"; extra = @("--fx=real", "--seed=2") },
  @{ name = "half"; extra = @("--fx=half") },
  @{ name = "xgs"; extra = @("--currencies=0") }
)
if ($NoUrban) {
  $variants += @{ name = "real_nourban"; extra = @("--fx=real") }
}

$start = Get-Date
Write-Output ("[q015] inicio " + $start.ToString("HH:mm:ss") + " processos=" + $variants.Count)
$procs = @()
foreach ($v in $variants) {
  $urban = if ($v.name -eq "real_nourban") { "--urban=0" } else { "--urban=1" }
  $out = "user://world_sweep/$OutPrefix$($v.name)_$Days.json"
  $args = @("--headless", "--path", $project, "--script", "res://tools/economy_world_test.gd",
    "--", "--ai=1", "--days=$Days", $urban, "--threads=1",
    "--baseline=$baseline", "--out=$out") + $v.extra
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
