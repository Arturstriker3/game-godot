# Bench do modo TURBO contra o relogio normal, na cena REAL e headless.
#
# Mede dias/segundo do jogo de verdade (autoload Economy, cena main_game) em
# quatro execucoes sequenciais:
#   1. cena real (com janela), normal  -> --bench-days=N --bench-speed=3
#   2. cena real (com janela), turbo   -> --turbo --bench-days=N
#   3. headless, normal
#   4. headless, turbo
# Cada uma tem de imprimir UMA linha
#   [bench] mode=... from_day=... to_day=... wall_s=... days_per_s=...
# que e extraida por regex e vira uma linha da tabela final.
#
# Uso:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/turbo_bench.ps1
#   powershell ... -File tools/turbo_bench.ps1 -NormalDays 2 -TurboDays 10
#   powershell ... -File tools/turbo_bench.ps1 -DaysPerFrame 8 -OutFile bench.json
#
# -DaysPerFrame (0 = deixa o padrao do turbo, 4) so entra na linha do turbo.
# -NormalDays usa a velocidade MAXIMA do relogio (speed 3 = 15 s/dia), entao
# cada dia normal custa ~15 s de parede: valores grandes demoram de verdade.
#
# Saida: 0 = as 4 linhas sairam; 1 = alguma nao saiu (imprime o tail do log) ou
# um Godot nao fechou em 10 min; 2 = erro de configuracao (godot nao achado).
# Nao toca na DLL, nao abre o editor e nao mata processo do editor.

param(
  [int]$TurboDays = 60,
  [int]$NormalDays = 4,
  [int]$DaysPerFrame = 0,
  [string]$Godot = "",
  [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$benchScene = "res://game/main_game.tscn"
$timeoutMs = 600000   # 10 min por execucao: o bench se encerra sozinho

if ($TurboDays -lt 1) { $TurboDays = 1 }
if ($NormalDays -lt 1) { $NormalDays = 1 }
if ($DaysPerFrame -lt 0) { $DaysPerFrame = 0 }

# --- godot ------------------------------------------------------------------
# Mesmo caminho de descoberta de tools/world_equivalence.ps1: processo aberto,
# depois uma busca rasa no OneDrive do usuario.
if (-not $Godot) {
  $editor = Get-Process -Name "Godot_v4.7.2-stable_win64" -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if ($editor) { $Godot = $editor.Path }
}
if (-not $Godot -or -not (Test-Path $Godot)) {
  $found = Get-ChildItem "$env:USERPROFILE\OneDrive" -Filter "Godot_v4.7.2-stable_win64.exe" `
    -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($found) { $Godot = $found.FullName }
}
if (-not $Godot -or -not (Test-Path $Godot)) {
  Write-Output "[bench] ERRO: godot nao encontrado (use -Godot <caminho>)"
  exit 2
}

$workDir = Join-Path $env:TEMP ("opengs_bench\" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

Write-Output ("[bench] root=" + $root)
Write-Output ("[bench] godot=" + $Godot)
Write-Output ("[bench] normal dias=" + $NormalDays + " (speed 3) | turbo dias=" + $TurboDays +
  $(if ($DaysPerFrame -gt 0) { " (lote " + $DaysPerFrame + ")" } else { " (lote padrao)" }))
Write-Output ("[bench] logs em " + $workDir)

# --- as quatro execucoes ----------------------------------------------------
$runs = @(
  [pscustomobject]@{
    Scene = "janela"; Mode = "normal"; Days = $NormalDays
    Args = @("--path", $root, "--", "--bench-days=$NormalDays", "--bench-speed=3")
  },
  [pscustomobject]@{
    Scene = "janela"; Mode = "turbo"; Days = $TurboDays
    Args = @("--path", $root, "--", "--turbo", "--bench-days=$TurboDays") +
      $(if ($DaysPerFrame -gt 0) { @("--turbo-days-per-frame=$DaysPerFrame") } else { @() })
  },
  [pscustomobject]@{
    Scene = "headless"; Mode = "normal"; Days = $NormalDays
    Args = @("--headless", "--path", $root, $benchScene, "--",
      "--bench-days=$NormalDays", "--bench-speed=3")
  },
  [pscustomobject]@{
    Scene = "headless"; Mode = "turbo"; Days = $TurboDays
    Args = @("--headless", "--path", $root, $benchScene, "--", "--turbo",
      "--bench-days=$TurboDays") +
      $(if ($DaysPerFrame -gt 0) { @("--turbo-days-per-frame=$DaysPerFrame") } else { @() })
  }
)

# `[bench] mode=normal from_day=1 to_day=2 wall_s=15.031 days_per_s=0.07`
$benchRegex = '\[bench\]\s+mode=(\w+)\s+from_day=(-?\d+)\s+to_day=(-?\d+)\s+wall_s=([0-9.]+)\s+days_per_s=([0-9.]+)'

$records = @()
$index = 0
foreach ($run in $runs) {
  $index++
  $stdout = Join-Path $workDir ("run" + $index + "." + $run.Scene + "." + $run.Mode + ".log")
  $stderr = Join-Path $workDir ("run" + $index + "." + $run.Scene + "." + $run.Mode + ".err.log")
  Write-Output ("[bench] " + $index + "/4 " + $run.Scene + " " + $run.Mode + " ...")

  $t0 = Get-Date
  $proc = Start-Process -FilePath $Godot -ArgumentList $run.Args -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  $finished = $proc.WaitForExit($timeoutMs)
  $elapsed = [int]((Get-Date) - $t0).TotalSeconds

  if (-not $finished) {
    Write-Output ("[bench] ERRO: o Godot nao fechou em " + [int]($timeoutMs / 1000) +
      "s (" + $run.Scene + " " + $run.Mode + "); encerrando o processo do bench")
    try { $proc.Kill() } catch { }
    try { $proc.WaitForExit(10000) | Out-Null } catch { }
    if (Test-Path $stdout) {
      Write-Output "[bench] ultimas linhas de ${stdout}:"
      Get-Content -LiteralPath $stdout -Tail 15 | ForEach-Object { Write-Output ("  " + $_) }
    }
    exit 1
  }
  Write-Output ("[bench] " + $index + "/4 " + $run.Scene + " " + $run.Mode + " terminou em " +
    $elapsed + "s")

  $line = ""
  if (Test-Path $stdout) {
    $match = Select-String -LiteralPath $stdout -Pattern $benchRegex | Select-Object -First 1
    if ($match) { $line = $match.Line.Trim() }
  }
  if (-not $line) {
    Write-Output ("[bench] ERRO: sem linha [bench] na execucao " + $index + " (" +
      $run.Scene + " " + $run.Mode + ")")
    foreach ($log in @($stdout, $stderr)) {
      if ((Test-Path $log) -and ((Get-Item $log).Length -gt 0)) {
        Write-Output "[bench] ultimas linhas de ${log}:"
        Get-Content -LiteralPath $log -Tail 20 | ForEach-Object { Write-Output ("  " + $_) }
      }
    }
    exit 1
  }

  $m = [regex]::Match($line, $benchRegex)
  $fromDay = [int]$m.Groups[2].Value
  $toDay = [int]$m.Groups[3].Value
  $wall = [double]::Parse($m.Groups[4].Value, [Globalization.CultureInfo]::InvariantCulture)
  $perSec = [double]::Parse($m.Groups[5].Value, [Globalization.CultureInfo]::InvariantCulture)
  $records += [pscustomobject]@{
    scene = $run.Scene
    mode = $m.Groups[1].Value
    requested_days = $run.Days
    from_day = $fromDay
    to_day = $toDay
    days = $toDay - $fromDay
    wall_s = $wall
    days_per_s = $perSec
    elapsed_s = $elapsed
    line = $line
  }
  Write-Output ("[bench]   " + $line)
}

# --- tabela -----------------------------------------------------------------
Write-Output ""
Write-Output "[bench] cena      modo    dias   wall_s   dias/s"
Write-Output "[bench] --------  ------  -----  -------  --------"
foreach ($record in $records) {
  $scene = $record.scene.PadRight(8)
  $mode = $record.mode.PadRight(6)
  $days = ([string]$record.days).PadLeft(5)
  $wall = $record.wall_s.ToString("0.000", [Globalization.CultureInfo]::InvariantCulture).PadLeft(7)
  $rate = $record.days_per_s.ToString("0.00", [Globalization.CultureInfo]::InvariantCulture).PadLeft(8)
  Write-Output ("[bench] " + $scene + "  " + $mode + "  " + $days + "  " + $wall + "  " + $rate)
}

# --- JSON opcional ----------------------------------------------------------
if ($OutFile) {
  $target = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
  $dir = Split-Path -Parent $target
  if ($dir -and -not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
  }
  $payload = [pscustomobject]@{
    generated_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    turbo_days = $TurboDays
    normal_days = $NormalDays
    days_per_frame = $DaysPerFrame
    records = $records
  }
  $text = ConvertTo-Json -InputObject $payload -Depth 6
  [System.IO.File]::WriteAllText($target, $text + "`n", (New-Object System.Text.UTF8Encoding($false)))
  Write-Output ("[bench] json escrito " + $target)
}

Write-Output ("[bench] OK: 4 linhas [bench] extraidas; logs em " + $workDir)
exit 0
