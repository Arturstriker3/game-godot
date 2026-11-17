# Equivalencia funcional A1-A5 (docs/plans/perf_pop_financas_plano.md).
#
# Roda o mundo real (60 d por padrao, IA e camada urbana ligadas, semente fixa)
# N vezes em sequencia, grava um JSON canonico por rodada e compara campo a
# campo com tools/world_equivalence_compare.py. Serve de rede de seguranca para
# os passos A1..A5: se um refactor mudar o mundo, o PASS vira FAIL e o caminho
# do primeiro campo divergente sai na tela.
#
# Uso:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/world_equivalence.ps1
#   powershell ... -File tools/world_equivalence.ps1 -Days 60 -Rounds 2 -Seed 0
#   powershell ... -File tools/world_equivalence.ps1 -Baseline docs/economy/refactor/baseline/A0_60_urban.json
#   powershell ... -File tools/world_equivalence.ps1 -Golden %TEMP%\a0.json [-Force]
#
# -Baseline aceita dois formatos, detectados pelo tamanho e pelas chaves:
#   (a) dump grande de --out -> comparacao campo a campo (comparador Python);
#   (b) meta-golden pequeno (save_sha256/status_sha256 no topo, como os de
#       docs/economy/refactor/baseline/) -> compara os dois shas e a config
#       (days/ai/urban/seed/threads). Nao carrega 110 MB no PowerShell.
#
# -Golden grava/atualiza um meta-golden pequeno numa unica rodada. Sem -Force
# nao sobrescreve arquivo existente (exit 2).
#
# Saida: JSONs e logs em -OutDir (default %TEMP%\opengs_equiv\<guid>).
# Exit: 0 = todas as comparacoes passaram; 1 = divergencia ou falha de
#       execucao; 2 = erro de configuracao (python/godot/arquivo/golden).
# Nao toca em DLL nem abre o editor.

param(
  [int]$Days = 60,
  [int]$Rounds = 2,
  [string]$Godot = "",
  [string]$OutDir = "",
  [string]$Baseline = "",
  [int]$Urban = 1,
  [int]$Ai = 1,
  [int]$Seed = 0,
  [string]$Python = "",
  [string]$Golden = "",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
$env:PYTHONIOENCODING = "utf-8"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script = Join-Path $root "tools\world_equivalence.gd"
$comparer = Join-Path $root "tools\world_equivalence_compare.py"

if (-not $OutDir) {
  $OutDir = Join-Path $env:TEMP ("opengs_equiv\" + [guid]::NewGuid().ToString("N"))
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# --- python (mesmo interpretador do PATH) -----------------------------------
$pythonCmd = Get-Command python -ErrorAction SilentlyContinue
if ($Python) {
  if (-not (Test-Path -LiteralPath $Python)) {
    Write-Output ("[equiv] ERRO: python informado nao existe: " + $Python)
    exit 2
  }
  $python = (Resolve-Path -LiteralPath $Python).Path
} elseif (-not $pythonCmd) {
  Write-Output "[equiv] ERRO: python nao encontrado no PATH"
  exit 2
} else {
  $python = $pythonCmd.Source
}
try {
  & $python --version *> $null
  if ($LASTEXITCODE -ne 0) { throw "exit $LASTEXITCODE" }
} catch {
  Write-Output ("[equiv] ERRO: '" + $python + "' nao executou (alias da Store ou PATH quebrado)")
  exit 2
}
if (-not (Test-Path $comparer)) {
  Write-Output "[equiv] ERRO: comparador ausente: $comparer"
  exit 2
}
if (-not (Test-Path $script)) {
  Write-Output "[equiv] ERRO: harness ausente: $script"
  exit 2
}

# --- godot ------------------------------------------------------------------
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
  Write-Output "[equiv] ERRO: godot nao encontrado (use -Godot <caminho>)"
  exit 2
}

if ($Rounds -lt 1) { $Rounds = 1 }

# --- modo golden ------------------------------------------------------------
$goldenPath = ""
if ($Golden) {
  $Rounds = 1
  $goldenPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Golden)
  if ((Test-Path -LiteralPath $goldenPath) -and -not $Force) {
    Write-Output ("[equiv] ERRO: golden ja existe (use -Force para sobrescrever): " + $goldenPath)
    exit 2
  }
  if ($Baseline) {
    Write-Output "[equiv] aviso: -Baseline ignorado no modo -Golden"
  }
}

Write-Output ("[equiv] root=" + $root)
Write-Output ("[equiv] godot=" + $Godot)
Write-Output ("[equiv] dias=" + $Days + " rodadas=" + $Rounds + " ai=" + $Ai +
  " urban=" + $Urban + " seed=" + $Seed)
Write-Output ("[equiv] saida=" + $OutDir)
if ($goldenPath) { Write-Output ("[equiv] golden=" + $goldenPath) }

$baselinePath = ""
if ($Baseline -and -not $goldenPath) {
  $resolved = Resolve-Path -LiteralPath $Baseline -ErrorAction SilentlyContinue
  if (-not $resolved) {
    Write-Output ("[equiv] ERRO: baseline inexistente: " + $Baseline)
    exit 2
  }
  $baselinePath = [string]$resolved
}

# --- rodadas ----------------------------------------------------------------
$files = @()
$metas = @()
$start = Get-Date
for ($i = 1; $i -le $Rounds; $i++) {
  $file = Join-Path $OutDir ("run" + $i + ".json")
  $metaFile = Join-Path $OutDir ("run" + $i + ".meta.json")
  $log = Join-Path $OutDir ("run" + $i + ".log")
  $errLog = Join-Path $OutDir ("run" + $i + ".err.log")
  $godotArgs = @(
    "--headless", "--path", $root,
    "--script", "res://tools/world_equivalence.gd",
    "--",
    "--days=$Days", "--ai=$Ai", "--urban=$Urban", "--seed=$Seed",
    "--out=$file", "--meta-out=$metaFile"
  )
  Write-Output ("[equiv] rodada " + $i + "/" + $Rounds + " ...")
  $t0 = Get-Date
  & $Godot @godotArgs 2> $errLog | Out-File -FilePath $log -Encoding utf8
  $code = $LASTEXITCODE
  $elapsed = [int]((Get-Date) - $t0).TotalSeconds
  Write-Output ("[equiv] rodada " + $i + " exit=" + $code + " em " + $elapsed + "s")

  if ($code -ne 0) {
    Write-Output "[equiv] FALHA: a rodada $i terminou com exit $code (nada a comparar)"
    if (Test-Path $log) {
      Write-Output "[equiv] ultimas linhas de ${log}:"
      Get-Content -LiteralPath $log -Tail 15 | ForEach-Object { Write-Output ("  " + $_) }
    }
    if ((Test-Path $errLog) -and (Get-Item $errLog).Length -gt 0) {
      Write-Output "[equiv] ultimas linhas de ${errLog}:"
      Get-Content -LiteralPath $errLog -Tail 15 | ForEach-Object { Write-Output ("  " + $_) }
    }
    exit 1
  }
  if (-not (Test-Path $file)) {
    Write-Output ("[equiv] FALHA: a rodada " + $i + " nao gravou " + $file)
    exit 1
  }
  if (-not (Test-Path $metaFile)) {
    Write-Output ("[equiv] FALHA: a rodada " + $i + " nao gravou " + $metaFile)
    exit 1
  }
  $files += $file
  $metas += $metaFile
}

# --- modo golden: grava e encerra -------------------------------------------
if ($goldenPath) {
  $goldenDir = Split-Path -Parent $goldenPath
  if ($goldenDir -and -not (Test-Path -LiteralPath $goldenDir)) {
    New-Item -ItemType Directory -Force -Path $goldenDir | Out-Null
  }
  Copy-Item -LiteralPath $metas[0] -Destination $goldenPath -Force
  $goldenObj = Get-Content -LiteralPath $goldenPath -Raw -Encoding UTF8 | ConvertFrom-Json
  Write-Output ("GOLDEN ESCRITO " + $goldenPath + " save_sha=" + $goldenObj.save_sha256 +
    " status_sha=" + $goldenObj.status_sha256)
  Write-Output ("[equiv] golden pequeno (" + (Get-Item -LiteralPath $goldenPath).Length +
    " B): dias=" + $Days + " ai=" + $Ai + " urban=" + $Urban + " seed=" + $Seed)
  exit 0
}

# --- deteccao do tipo de baseline -------------------------------------------
# meta-golden e sempre pequeno (< 256 KB); dump de mundo tem dezenas de MB.
function Read-MetaGolden([string]$path) {
  $item = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
  if (-not $item) { return $null }
  if ($item.Length -gt 262144) { return $null }
  $obj = $null
  try {
    $obj = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
  } catch {
    return $null
  }
  if ($null -eq $obj) { return $null }
  $names = @($obj.PSObject.Properties.Name)
  if (($names -contains "save_sha256") -and ($names -contains "status_sha256")) { return $obj }
  return $null
}

function Compare-MetaGolden([string]$runMetaPath, [string]$goldenFile, $golden) {
  $lines = @()
  $ok = $true
  $failReasons = @()
  $run = Get-Content -LiteralPath $runMetaPath -Raw -Encoding UTF8 | ConvertFrom-Json

  # os goldens S0 guardam a config em .meta; o --meta-out deste harness e plano.
  $goldenNames = @($golden.PSObject.Properties.Name)
  if ($goldenNames -contains "meta") { $cfg = $golden.meta } else { $cfg = $golden }
  $cfgNames = @($cfg.PSObject.Properties.Name)

  $compared = @()
  foreach ($field in @("days", "ai", "urban", "seed", "threads")) {
    if ($cfgNames -contains $field) { $gv = $cfg.$field }
    elseif ($goldenNames -contains $field) { $gv = $golden.$field }
    else { continue }
    $rv = $run.$field
    if ([string]$gv -ne [string]$rv) {
      $ok = $false
      $failReasons += ("config " + $field + ": golden=" + $gv + " run=" + $rv)
    }
    $compared += $field
  }

  $gs = [string]$golden.save_sha256
  $gst = [string]$golden.status_sha256
  $rs = [string]$run.save_sha256
  $rst = [string]$run.status_sha256
  if ($gs -ne $rs) {
    $ok = $false
    $failReasons += ("save_sha256: golden=" + $gs + " run=" + $rs)
  }
  if ($gst -ne $rst) {
    $ok = $false
    $failReasons += ("status_sha256: golden=" + $gst + " run=" + $rst)
  }

  $cfgText = "nenhum (golden sem campos de config)"
  if ($compared.Count -gt 0) { $cfgText = ($compared -join ",") }
  $lines += ("[equiv] golden: " + $goldenFile)
  $lines += ("[equiv] golden: config comparada = " + $cfgText + " | save_sha=" + $rs +
    " | status_sha=" + $rst)
  foreach ($reason in $failReasons) { $lines += ("[equiv]   " + $reason) }

  return [pscustomobject]@{
    Ok = $ok
    Lines = $lines
    FirstFail = $(if ($failReasons.Count -gt 0) { $failReasons[0] } else { "" })
    SaveSha = $rs
    StatusSha = $rst
  }
}

# --- comparacoes ------------------------------------------------------------
$comparisons = 0
$failures = 0

# 1) determinismo: run1 vs runN (dumps grandes, campo a campo).
for ($i = 2; $i -le $files.Count; $i++) {
  $labelA = $files[0]
  $labelB = $files[$i - 1]
  $label = "run1 vs run" + $i
  Write-Output ("[equiv] --- " + $label)
  $output = @()
  $code = 2
  try {
    # meta.generated_utc e relogio de parede, nao mundo: fica fora da comparacao.
    $output = & $python $comparer $labelA $labelB --max-diffs 20 --ignore meta.generated_utc 2>&1
    $code = $LASTEXITCODE
  } catch {
    Write-Output ("[equiv] ERRO: falha ao executar o comparador em " + $label + ": " + $_.Exception.Message)
    exit 2
  }
  $output | ForEach-Object { Write-Output $_ }
  $comparisons++
  if ($code -eq 0) {
    Write-Output ("[equiv] OK " + $label + ": 0 diferencas")
  } elseif ($code -eq 1) {
    $failures++
    $first = $output | Where-Object { $_ -match '^\[equiv\] divergencia 1: ' } |
      Select-Object -First 1
    if ($first) {
      Write-Output ("[equiv] FAIL " + $label + ": " + $first)
    } else {
      Write-Output ("[equiv] FAIL " + $label + ": divergencia(s) sem caminho legivel")
    }
  } else {
    Write-Output ("[equiv] ERRO: comparador retornou exit " + $code + " em " + $label)
    exit 2
  }
}

# 2) baseline: meta-golden (shas + config) ou dump grande (campo a campo).
if ($baselinePath) {
  $metaGolden = Read-MetaGolden $baselinePath
  if ($metaGolden) {
    Write-Output "[equiv] --- run1 vs baseline (meta-golden)"
    $result = Compare-MetaGolden $metas[0] $baselinePath $metaGolden
    $result.Lines | ForEach-Object { Write-Output $_ }
    $comparisons++
    if ($result.Ok) {
      Write-Output ("[equiv] OK run1 vs baseline: shas e config iguais (save=" +
        $result.SaveSha + " status=" + $result.StatusSha + ")")
    } else {
      $failures++
      Write-Output ("[equiv] FAIL run1 vs baseline: " + $result.FirstFail)
    }
  } else {
    Write-Output "[equiv] --- run1 vs baseline (dump)"
    $output = @()
    $code = 2
    try {
      $output = & $python $comparer $files[0] $baselinePath --max-diffs 20 --ignore meta.generated_utc 2>&1
      $code = $LASTEXITCODE
    } catch {
      Write-Output ("[equiv] ERRO: falha ao executar o comparador no baseline: " + $_.Exception.Message)
      exit 2
    }
    $output | ForEach-Object { Write-Output $_ }
    $comparisons++
    if ($code -eq 0) {
      Write-Output "[equiv] OK run1 vs baseline: 0 diferencas"
    } elseif ($code -eq 1) {
      $failures++
      $first = $output | Where-Object { $_ -match '^\[equiv\] divergencia 1: ' } |
        Select-Object -First 1
      if ($first) {
        Write-Output ("[equiv] FAIL run1 vs baseline: " + $first)
      } else {
        Write-Output "[equiv] FAIL run1 vs baseline: divergencia(s) sem caminho legivel"
      }
    } else {
      Write-Output ("[equiv] ERRO: comparador retornou exit " + $code + " no baseline")
      exit 2
    }
  }
}

if ($comparisons -eq 0) {
  Write-Output "[equiv] so uma rodada executada e sem baseline: nada a comparar"
  Write-Output ("[equiv] PASS (vazio) - json em " + $files[0])
  exit 0
}

$total = [int]((Get-Date) - $start).TotalSeconds
Write-Output ("[equiv] json/logs em " + $OutDir)
if ($failures -gt 0) {
  Write-Output ("[equiv] FAIL: " + $failures + " comparacao(oes) com divergencia (" + $total + "s)")
  exit 1
}
Write-Output ("[equiv] PASS: 0 diferencas em " + $comparisons + " comparacao(oes) (" + $total + "s)")
exit 0
