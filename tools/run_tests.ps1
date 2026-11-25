# run_tests.ps1 -- canonical headless test runner for OpenGS Game.
#
# WHY: test tasks used to open several Godot processes at once, some got stuck
# on the crash/ALERT dialog after `--script` was pointed at a scene, and memory
# was not coming back. This script is the ONLY supported way to run tests:
#   - one Godot test process on the whole machine (global lock + foreign check)
#   - free RAM/disk floor checked before every test
#   - `--script` only for SceneTree/MainLoop scripts; scenes run as scenes
#   - hard timeout, process-tree kill, WerFault cleanup, exit-code recording
#   - JSON + per-test logs in a private temp folder on G: (never C:)
#
# TEMP ROOT (disk rule): every run artifact lives under -TmpRoot, whose default
# is G:\opengs_tmp -- runs (opengs_test_run_*), the lock, per-test logs and the
# summary. Each Godot child gets TEMP/TMP = <TmpRoot>\godot_tmp and
# OPENGS_TMP = <TmpRoot>, so the engine and the test scripts do not spill big
# files on C:. If the tmp drive/dir is not usable the runner falls back to
# $env:TEMP with a warning. Override per invocation: -TmpRoot "D:\somewhere".
#
# The file is intentionally ASCII-only: accented Windows paths (OneDrive
# "Area de Trabalho") must not break parsing/encoding on PowerShell 5.1.
#
# Usage (PowerShell 5.1+):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -All
#   ... -List services_test,economy_integration_test
#   ... -Pattern "*economy*" -TimeoutSec 1200
#   ... -TimeoutOverrides "economy_long_run_test=3600,world_equivalence_test=1800"
#   ... -TmpRoot "G:\opengs_tmp"
#
# Layered validation (pre-merge gate):
#   ... -Tier quick                 # testes impactados pelo diff + sentinelas baratas
#   ... -Tier pr                    # quick + rodada c1 de 90 dias com digest
#   ... -Tier nightly               # -All + rodada c1 de 365 dias
#   ... -Tier quick -Base main      # diff contra outra ref (padrao: balance/global-resources)
#   ... -Tier quick -DryRun         # so mostra o que rodaria; lista as checagens
#   ... -Parallel 3                 # checagens de CLI em paralelo (nunca Godot; padrao 3)
# -Tier usa tools/test_impact_map.json (caminho alterado -> testes) e sempre inclui as
# sentinelas declaradas la. -List/-Pattern continuam funcionando e, com -Tier, sao somados
# as sentinelas. -All sem -Tier mantem exatamente o comportamento antigo.
# -Tier quick/pr roda PRIMEIRO as checagens de CLI do mapa (`cli_checks`), sem
# Godot: `sim_unit_tests` (testes unitarios C++ do nucleo, FASE 2) roda em
# milissegundos e, se falhar, o tier aborta antes de qualquer teste Godot.
# Se o executavel nao estiver compilado a checagem vira SKIP. Como compilar e
# escrever teste: docs/testing/cpp_unit_tests.md. Qualquer mudanca em
# gdextension/src/** cobre esse alvo (ele roda sempre no quick, nao por diff).
# Mudar a propria infra de testes (tools/run_tests.ps1, tools/test_impact_map.json,
# tools/tmp_paths.gd) invalida a selecao: o mapa manda rodar a bateria inteira (-All).
# Qualquer arquivo em gdextension/src/ seleciona o conjunto amplo de nucleo (core_set)
# e imprime o aviso de -Tier nightly antes do merge.
#
# Exit codes:
#   0 all green
#   1 at least one FAIL / CRASH / TIMEOUT
#   2 setup error (Godot not found, unknown selection, nothing to run)
#   3 lock wait exceeded LockWaitSec
#   4 resource floor not reached within ResourceWaitSec
#
# Never run this in parallel with itself or with another Godot test process.
# The lock file is <TmpRoot>\opengs_godot_test.lock (PID inside) and is always
# released in the finally block.

[CmdletBinding()]
param(
    [string[]]$List = @(),
    [string]$Pattern = "",
    [switch]$All,
    [int]$TimeoutSec = 900,
    [string[]]$TimeoutOverrides = @(),
    [int]$LockWaitSec = 3600,
    [int]$ResourceWaitSec = 300,
    [int]$ImportTimeoutSec = 1800,
    [double]$MinFreeRamGB = 4.0,
    [double]$MinFreeDiskGB = 10.0,
    [string]$Godot = "",
    [string]$ProjectRoot = "",
    [string]$TmpRoot = "G:\opengs_tmp",
    [string]$OutDir = "",
    [string]$Tier = "",
    [string]$Base = "balance/global-resources",
    [string]$ImpactMap = "",
    [int]$Parallel = 3,
    [switch]$DryRun,
    [switch]$KeepLogs
)

$ErrorActionPreference = "Stop"

$script:Root = ""
$script:GodotExe = ""
$script:TmpRoot = ""
$script:GodotTmpDir = ""
$script:LockPath = ""
$script:LockStream = $null
$script:LockOwned = $false
$script:OutDir = ""
$script:LogDir = ""
$script:Results = @()
$script:SystemDiskWarned = $false
$script:BaseRef = ""
$script:ChangedFiles = @()
$script:SelectionReasons = [ordered]@{}
$script:SelectionNeedsNightly = $false
$script:ImpactMapPath = ""

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

function Quote-Arg([string]$value) {
    if ($value -match '\s') { return '"' + $value + '"' }
    return $value
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
}

function Get-DriveRoot([string]$Path) {
    try {
        $root = [System.IO.Path]::GetPathRoot($Path)
        if ($root) { return $root }
    } catch {
    }
    return "C:\"
}

function Get-FreeDiskGB([string]$Path) {
    $root = Get-DriveRoot $Path
    try {
        $di = New-Object System.IO.DriveInfo($root)
        return [math]::Round(($di.AvailableFreeSpace / 1GB), 2)
    } catch {
        $name = $root.TrimEnd('\', '/').TrimEnd(':')
        if (-not $name) { $name = "C" }
        try {
            $d = Get-PSDrive -Name $name -ErrorAction Stop
            return [math]::Round(($d.Free / 1GB), 2)
        } catch {
            return 0.0
        }
    }
}

# The tmp root is where EVERY run artifact goes. Default G:\opengs_tmp; if the
# disk/dir is not writable (G: missing on another machine) fall back to
# $env:TEMP with a warning instead of failing the battery.
function Resolve-TmpRoot([string]$Requested) {
    $candidate = $Requested
    if (-not $candidate) { $candidate = "G:\opengs_tmp" }
    try {
        if (-not (Test-Path -LiteralPath $candidate)) {
            New-Item -ItemType Directory -Path $candidate -Force -ErrorAction Stop | Out-Null
        }
        $probe = Join-Path $candidate (".write_probe_" + $PID + ".tmp")
        [System.IO.File]::WriteAllText($probe, "ok")
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        return (Resolve-Path -LiteralPath $candidate).Path
    } catch {
        $fallback = Join-Path $env:TEMP "opengs_test_tmp"
        Write-Host ("[tmp] aviso: nao consegui usar '{0}' ({1}); usando '{2}'." -f `
            $candidate, $_.Exception.Message, $fallback)
        if (-not (Test-Path -LiteralPath $fallback)) {
            New-Item -ItemType Directory -Path $fallback -Force | Out-Null
        }
        return $fallback
    }
}

# Every Godot child runs with TEMP/TMP on the tmp drive and OPENGS_TMP set, so
# engine scratch files and the test scripts' own artifacts leave C: alone.
function Start-GodotProcess {
    param([string[]]$Arguments, [string]$OutLog, [string]$ErrLog)
    $prevTemp = $env:TEMP
    $prevTmp = $env:TMP
    $prevOpenGsTmp = $env:OPENGS_TMP
    try {
        if ($script:GodotTmpDir) {
            $env:TEMP = $script:GodotTmpDir
            $env:TMP = $script:GodotTmpDir
            $env:OPENGS_TMP = $script:TmpRoot
        }
        $proc = Start-Process -FilePath $script:GodotExe -ArgumentList $Arguments `
            -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog `
            -NoNewWindow -PassThru -ErrorAction Stop
        # Keep the OS handle: without it PowerShell 5.1 can report ExitCode as
        # $null for a redirected process that exits quickly.
        $null = $proc.Handle
        return $proc
    } finally {
        $env:TEMP = $prevTemp
        $env:TMP = $prevTmp
        if ($null -eq $prevOpenGsTmp) {
            Remove-Item Env:\OPENGS_TMP -ErrorAction SilentlyContinue
        } else {
            $env:OPENGS_TMP = $prevOpenGsTmp
        }
    }
}

function Convert-ToUInt32([int]$code) {
    if ($code -lt 0) { return [uint32]([int64]$code + 4294967296) }
    return [uint32]$code
}

function Get-ExitCodeHex([int]$code) {
    $u = Convert-ToUInt32 -code $code
    return ("0x{0:X8}" -f $u)
}

function Test-MemoryAccessCrash([int]$code) {
    # 0xC0000005 = STATUS_ACCESS_VIOLATION (null/foreign memory access).
    return ((Convert-ToUInt32 -code $code) -eq [uint32]3221225477)
}

function Get-LogTail([string]$Path, [int]$Lines = 15, [int]$MaxCharsPerLine = 300, [int]$MaxTotalChars = 4000) {
    # Le SO os ultimos 64 KB do arquivo: um teste que imprime uma unica linha
    # gigante (dump de JSON) nao pode ser carregado inteiro para a memoria do
    # runner -- era isso que estourava o summary com OutOfMemoryException.
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $fs = $null
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $length = $fs.Length
        if ($length -le 0) { return @() }
        $take = [int][math]::Min([int64]65536, $length)
        $fs.Seek(-[int64]$take, [System.IO.SeekOrigin]::End) | Out-Null
        $buffer = New-Object byte[] $take
        $read = $fs.Read($buffer, 0, $take)
        if ($read -le 0) { return @() }
        $text = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    } catch {
        return @()
    } finally {
        if ($null -ne $fs) { try { $fs.Dispose() } catch { } }
    }

    $all = @($text -split "`r?`n")
    $start = [math]::Max(0, $all.Count - $Lines)
    $out = New-Object System.Collections.Generic.List[string]
    $used = 0
    for ($i = $start; $i -lt $all.Count; $i++) {
        $line = [string]$all[$i]
        if ($line.Length -gt $MaxCharsPerLine) {
            $line = $line.Substring(0, $MaxCharsPerLine) + "...[truncado]"
        }
        if (($used + $line.Length) -gt $MaxTotalChars) { break }
        $out.Add($line)
        $used += $line.Length
    }
    return $out.ToArray()
}

# ---------------------------------------------------------------------------
# Godot binary discovery (ASCII-safe: never hardcode the accented path)
# ---------------------------------------------------------------------------

function Find-GodotExe {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        throw "Godot nao encontrado no caminho informado: $Explicit"
    }
    if ($env:OGS_GODOT -and (Test-Path -LiteralPath $env:OGS_GODOT)) { return $env:OGS_GODOT }

    $roots = @()
    if ($env:USERPROFILE) {
        $roots += (Join-Path $env:USERPROFILE "OneDrive")
        $roots += (Join-Path $env:USERPROFILE "Desktop")
        $roots += (Join-Path $env:USERPROFILE "Downloads")
    }
    $roots += "C:\tools"
    $roots += "C:\Godot"

    $patterns = @("Godot_v4.7*stable_win64.exe", "Godot_v4*stable_win64.exe", "Godot_v4*.exe")
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($p in $patterns) {
            $hit = @(Get-ChildItem -LiteralPath $root -Filter $p -File -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1)
            if ($hit.Count -gt 0) { return $hit[0].FullName }
            $sub = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
                ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Filter $p -File -ErrorAction SilentlyContinue } |
                Sort-Object Name -Descending | Select-Object -First 1)
            if ($sub.Count -gt 0) { return $sub[0].FullName }
        }
    }

    $cmd = Get-Command godot -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    throw "Godot 4.7 nao encontrado. Use -Godot <caminho> ou defina OGS_GODOT."
}

# ---------------------------------------------------------------------------
# Test catalog: SceneTree script vs scene
# ---------------------------------------------------------------------------

function Test-IsSceneTreeScript {
    param([string]$Path)
    try {
        $head = @(Get-Content -LiteralPath $Path -TotalCount 60 -ErrorAction Stop)
    } catch {
        return $false
    }
    foreach ($line in $head) {
        if ($line -match '^\s*extends\s+([A-Za-z0-9_\.]+)') {
            $base = $Matches[1]
            if ($base -match 'SceneTree' -or $base -match 'MainLoop') { return $true }
            return $false
        }
    }
    return $false
}

function Get-TestCatalog {
    param([string]$Root)
    $tools = Join-Path $Root "tools"
    if (-not (Test-Path -LiteralPath $tools)) { throw "pasta tools/ nao encontrada em $Root" }

    $scripts = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $tools -Filter "*_test.gd" -File -ErrorAction Stop)) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
        $scripts[$base] = $f.FullName
    }
    $scenes = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $tools -Filter "*_test.tscn" -File -ErrorAction Stop)) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
        $scenes[$base] = $f.FullName
    }

    $names = @(@($scripts.Keys) + @($scenes.Keys) | Sort-Object -Unique)
    $catalog = @()
    foreach ($name in $names) {
        $entry = [pscustomobject]@{
            Name = $name
            Mode = ""
            ResPath = ""
            SkipReason = ""
        }
        if ($scripts.ContainsKey($name)) {
            if (Test-IsSceneTreeScript -Path $scripts[$name]) {
                $entry.Mode = "script"
                $entry.ResPath = "res://tools/$name.gd"
            } elseif ($scenes.ContainsKey($name)) {
                # Node/Node3D scripts must run through their scene, never --script.
                $entry.Mode = "scene"
                $entry.ResPath = "res://tools/$name.tscn"
            } else {
                $entry.Mode = "skip"
                $entry.SkipReason = "script nao extends SceneTree/MainLoop e nao tem .tscn"
            }
        } else {
            # Scene-only test (script lives elsewhere, e.g. embedded).
            $entry.Mode = "scene"
            $entry.ResPath = "res://tools/$name.tscn"
        }
        $catalog += $entry
    }
    return @($catalog)
}

function Select-Tests {
    param($Catalog, [string[]]$Names, [string]$NamePattern)
    if ($Names -and $Names.Count -gt 0) {
        $picked = @()
        foreach ($raw in $Names) {
            if (-not $raw) { continue }
            $n = $raw.Trim()
            $n = $n -replace '^tools[\\/]', ''
            $n = $n -replace '\.(gd|tscn)$', ''
            $hit = @($Catalog | Where-Object { $_.Name -ieq $n })
            if ($hit.Count -eq 0) { throw "teste desconhecido: $raw" }
            $picked += $hit[0]
        }
        return @($picked | Sort-Object Name -Unique)
    }
    if ($NamePattern) {
        $picked = @($Catalog | Where-Object { $_.Name -like $NamePattern })
        if ($picked.Count -eq 0) { throw "nenhum teste casa com o padrao '$NamePattern'" }
        return $picked
    }
    return @($Catalog)
}

# ---------------------------------------------------------------------------
# Layered selection (-Tier quick|pr|nightly): git diff -> impact map -> tests
# ---------------------------------------------------------------------------

function Get-TierRank([string]$Name) {
    switch ($Name) {
        "quick" { return 0 }
        "pr" { return 1 }
        "nightly" { return 2 }
        default { return -1 }
    }
}

# test_sets may reference other sets by name (e.g. core_set -> broad_economy).
function Expand-TestNames {
    param($Map, [string[]]$Tokens)
    $out = New-Object System.Collections.Generic.List[string]
    $seen = New-Object System.Collections.Generic.HashSet[string]
    $guard = New-Object System.Collections.Generic.HashSet[string]
    $queue = New-Object System.Collections.Generic.Queue[string]
    foreach ($t in $Tokens) {
        if ($t) { $queue.Enqueue([string]$t) }
    }
    while ($queue.Count -gt 0) {
        $tok = $queue.Dequeue()
        if ($tok -eq "ALL") {
            if (-not $seen.Contains($tok)) { [void]$seen.Add($tok); [void]$out.Add($tok) }
            continue
        }
        $isSet = $false
        if ($Map.test_sets) {
            $isSet = ($Map.test_sets.PSObject.Properties.Name -contains $tok)
        }
        if ($isSet) {
            if ($guard.Contains($tok)) { continue }
            [void]$guard.Add($tok)
            foreach ($sub in @($Map.test_sets.$tok)) {
                if ($sub) { $queue.Enqueue([string]$sub) }
            }
            continue
        }
        if (-not $seen.Contains($tok)) {
            [void]$seen.Add($tok)
            [void]$out.Add($tok)
        }
    }
    return @($out)
}

# Changed paths = committed since <Base> + working tree (staged/unstaged) + untracked.
# A missing base falls back to HEAD with a warning instead of aborting.
function Get-ChangedFiles {
    param([string]$Root, [string]$BaseRef)
    $files = New-Object System.Collections.Generic.List[string]
    $baseUsed = $BaseRef
    $baseOk = $false
    # Native stderr goes to a file (not $null): on Windows git prints CRLF
    # normalization warnings, and with $ErrorActionPreference=Stop a redirected
    # native stderr line can abort/blank the whole pipeline in PowerShell 5.1.
    $errRoot = $script:TmpRoot
    if (-not $errRoot) { $errRoot = $env:TEMP }
    $errFile = Join-Path $errRoot ("opengs_git_" + $PID + ".err")
    # Native stderr (git CRLF warnings on Windows) must not become a terminating
    # error: with $ErrorActionPreference=Stop, PS 5.1 aborts the pipeline and the
    # diff output is lost. Continue locally and send stderr to a file.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $null = & git -C $Root rev-parse --verify --quiet ("{0}^{{commit}}" -f $BaseRef) 2> $errFile
        if ($LASTEXITCODE -eq 0) { $baseOk = $true }
    } catch {
        $baseOk = $false
    }
    if (-not $baseOk) {
        Write-Host "[tier] base '$BaseRef' nao encontrada; usando HEAD."
        $baseUsed = "HEAD"
    }
    try {
        foreach ($f in @(& git -C $Root diff --name-only $baseUsed -- 2> $errFile)) {
            if ($f) { $files.Add([string]$f) }
        }
    } catch {
    }
    try {
        foreach ($f in @(& git -C $Root ls-files --others --exclude-standard 2> $errFile)) {
            if ($f) { $files.Add([string]$f) }
        }
    } catch {
    }
    $ErrorActionPreference = $prevEap
    Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue
    # Godot import artifacts (.uid/.import/.godot) are noise for impact selection.
    $clean = @()
    foreach ($f in $files) {
        $p = ([string]$f) -replace '\\', '/'
        if ($p -match '(^\.godot/|\.uid$|\.import$|\.tmp$|\.log$)') { continue }
        if ($p -match '^gdextension/([Bb]in|build[^/]*)/') { continue }
        $clean += $p
    }
    return @($clean | Sort-Object -Unique)
}

# First matching rule wins. A tools/<name>_test.gd change selects that test.
# No match (or a tools/ helper) falls back to default_tests, except when a rule
# matched on purpose with an empty list (helper-only change).
function Get-ImpactSelection {
    param($Map, [string[]]$Changed)
    $names = New-Object System.Collections.Generic.List[string]
    $reasons = [ordered]@{}
    $needsNightly = $false
    $coreTouched = $false
    foreach ($raw in $Changed) {
        $p = ([string]$raw).Trim()
        if (-not $p) { continue }
        $p = $p -replace '\\', '/'
        # Any C++/core source under gdextension/src/ earns an explicit nightly hint.
        if ($p -match '^gdextension/src/') { $coreTouched = $true }
        $matched = @()
        $ruleMatched = $false
        if ($p -match '^tools/([A-Za-z0-9_]+)_test\.(gd|tscn)$') {
            $matched = @($Matches[1])
            $ruleMatched = $true
        } else {
            foreach ($rule in @($Map.rules)) {
                if (-not $rule.match) { continue }
                if ($p -match $rule.match) {
                    $ruleMatched = $true
                    $matched = @(Expand-TestNames -Map $Map -Tokens @($rule.tests))
                    if ($rule.requires_nightly) { $needsNightly = $true }
                    break
                }
            }
        }
        if (-not $ruleMatched) {
            $matched = @(Expand-TestNames -Map $Map -Tokens @($Map.default_tests))
        }
        foreach ($n in $matched) {
            if (-not $n) { continue }
            $key = [string]$n
            if ($key -eq "ALL") { $key = "__ALL__" }
            if (-not $names.Contains($key)) { [void]$names.Add($key) }
            if (-not $reasons.Contains($key)) { $reasons[$key] = @() }
            if (-not ($reasons[$key] -contains $p)) { $reasons[$key] = @($reasons[$key]) + $p }
        }
    }
    return [pscustomobject]@{
        Names = @($names)
        Reasons = $reasons
        NeedsNightly = $needsNightly
        CoreTouched = $coreTouched
    }
}

function Read-ImpactMap([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "mapa de impacto nao encontrado: $Path"
    }
    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $map = $raw | ConvertFrom-Json
    if ($null -eq $map.rules) { throw "mapa de impacto invalido (sem 'rules'): $Path" }
    return $map
}

# ---------------------------------------------------------------------------
# CLI checks: the only parallel path. Never starts Godot.
# ---------------------------------------------------------------------------

function Get-CliChecks {
    param($Map, [string]$TierName)
    $tierRank = Get-TierRank $TierName
    if ($tierRank -lt 0 -or -not $Map.cli_checks) { return @() }
    return @($Map.cli_checks | Where-Object {
        (Get-TierRank ([string]$_.tier)) -le $tierRank
    })
}

function Invoke-CliChecks {
    param($Checks, [int]$MaxParallel, [string]$Root)
    $records = @()
    if (-not $Checks -or $Checks.Count -eq 0) {
        Write-Host "[cli] nenhuma checagem de CLI para este tier."
        return @()
    }
    $runnable = @()
    foreach ($c in $Checks) {
        $missing = @()
        foreach ($req in @($c.requires)) {
            if (-not $req) { continue }
            $full = Join-Path $Root ([string]$req)
            if (-not (Test-Path -LiteralPath $full)) { $missing += [string]$req }
        }
        if ($missing.Count -gt 0) {
            $records += [ordered]@{
                name = [string]$c.name
                mode = "cli"
                res_path = ""
                status = "SKIP"
                exit_code = $null
                exit_hex = ""
                duration_s = 0
                log_out = ""
                log_err = ""
                tail_out = @()
                tail_err = @("requer ausente: " + ($missing -join ", "))
            }
            Write-Host ("[skip]  {0}: requer ausente ({1})" -f $c.name, ($missing -join ", "))
            continue
        }
        $runnable += $c
    }
    if ($runnable.Count -eq 0) { return @($records) }

    $max = [math]::Max(1, $MaxParallel)
    Write-Host ("[cli] {0} checagem(ns), paralelismo {1} (sem Godot)." -f $runnable.Count, $max)
    $prevOpenGsTmp = $env:OPENGS_TMP
    $env:OPENGS_TMP = $script:TmpRoot
    try {
        for ($i = 0; $i -lt $runnable.Count; $i += $max) {
            $batch = @($runnable[$i..([math]::Min($i + $max - 1, $runnable.Count - 1))])
            $procs = @()
            foreach ($c in $batch) {
                $name = [string]$c.name
                $outLog = Join-Path $script:LogDir "$name.out.log"
                $errLog = Join-Path $script:LogDir "$name.err.log"
                $cmd = [string]$c.command
                # Comando relativo COM separador resolve contra a raiz do
                # projeto; nome puro (ex.: cmd.exe) continua resolvido pelo PATH.
                if ($cmd -and -not [System.IO.Path]::IsPathRooted($cmd) -and
                    ($cmd -match '[\\/]')) {
                    $cmd = Join-Path $Root $cmd
                }
                $argList = @()
                foreach ($a in @($c.args)) { if ($null -ne $a) { $argList += (Quote-Arg ([string]$a)) } }
                $started = Get-Date
                try {
                    # ArgumentList vazio faz Start-Process falhar; so passa a
                    # chave quando ha argumentos (ex.: sim_unit_tests sem args).
                    $startParams = @{
                        FilePath = $cmd
                        WorkingDirectory = $Root
                        RedirectStandardOutput = $outLog
                        RedirectStandardError = $errLog
                        NoNewWindow = $true
                        PassThru = $true
                        ErrorAction = "Stop"
                    }
                    if ($argList.Count -gt 0) { $startParams.ArgumentList = $argList }
                    $proc = Start-Process @startParams
                    $null = $proc.Handle
                    $procs += [pscustomobject]@{
                        Check = $c; Proc = $proc; Started = $started
                        OutLog = $outLog; ErrLog = $errLog
                        TimeoutSec = $(if ($c.timeout_s) { [int]$c.timeout_s } else { 300 })
                    }
                } catch {
                    $records += [ordered]@{
                        name = $name
                        mode = "cli"
                        res_path = ""
                        status = "FAIL"
                        exit_code = $null
                        exit_hex = "start-error"
                        duration_s = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
                        log_out = $outLog
                        log_err = $errLog
                        tail_out = @()
                        tail_err = @([string]$_.Exception.Message)
                    }
                    Write-Host ("[fail]  {0} (start-error)" -f $name)
                }
            }
            foreach ($p in $procs) {
                $deadline = $p.Started.AddSeconds($p.TimeoutSec)
                $timedOut = $false
                while (-not $p.Proc.HasExited) {
                    if ((Get-Date) -ge $deadline) { $timedOut = $true; break }
                    Start-Sleep -Milliseconds 250
                }
                if ($timedOut) {
                    Stop-ProcessTree -RootPid $p.Proc.Id
                }
                $code = $null
                if (-not $timedOut) {
                    try { $code = $p.Proc.ExitCode } catch { }
                    if ($null -eq $code) { try { $p.Proc.Refresh(); $code = $p.Proc.ExitCode } catch { } }
                }
                try { $p.Proc.Dispose() } catch { }
                $status = "FAIL"
                if ($timedOut) {
                    $status = "TIMEOUT"
                } elseif ($null -ne $code -and $code -eq 0) {
                    $status = "OK"
                }
                $rec = [ordered]@{
                    name = [string]$p.Check.name
                    mode = "cli"
                    res_path = ""
                    status = $status
                    exit_code = $code
                    exit_hex = ""
                    duration_s = [math]::Round(((Get-Date) - $p.Started).TotalSeconds, 2)
                    log_out = $p.OutLog
                    log_err = $p.ErrLog
                    tail_out = @()
                    tail_err = @()
                }
                if ($null -ne $code) { $rec.exit_hex = Get-ExitCodeHex -code ([int]$code) }
                if ($timedOut) { $rec.exit_hex = "timeout" }
                if ($status -ne "OK") {
                    $rec.tail_out = Get-LogTail -Path $p.OutLog
                    $rec.tail_err = Get-LogTail -Path $p.ErrLog
                }
                $records += $rec
                Write-Host ("[{0,-7}] {1} (cli) {2}s" -f $status.ToLower(), $rec.name, $rec.duration_s)
            }
        }
    } finally {
        if ($null -eq $prevOpenGsTmp) {
            Remove-Item Env:\OPENGS_TMP -ErrorAction SilentlyContinue
        } else {
            $env:OPENGS_TMP = $prevOpenGsTmp
        }
    }
    return @($records)
}

# ---------------------------------------------------------------------------
# Global lock (one runner at a time)
# ---------------------------------------------------------------------------

function Try-OpenLockFile {
    try {
        return [System.IO.File]::Open(
            $script:LockPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
    } catch {
        return $null
    }
}

function Get-LockOwnerPid {
    try {
        $txt = [System.IO.File]::ReadAllText($script:LockPath)
        if ($txt -match 'pid=(\d+)') { return [int]$Matches[1] }
    } catch {
    }
    return 0
}

function Acquire-TestLock {
    param([int]$MaxWaitSec)
    $start = Get-Date
    $announced = $false
    while ($true) {
        $fs = Try-OpenLockFile
        if ($fs) {
            $txt = "pid=$PID`nstarted=$((Get-Date).ToString('o'))`nuser=$env:USERNAME`nproject=$($script:Root)`n"
            $bytes = [System.Text.Encoding]::ASCII.GetBytes($txt)
            $fs.Write($bytes, 0, $bytes.Length)
            $fs.Flush()
            # Close the handle on purpose: the file must stay readable so a
            # waiting runner can print the owner PID. Mutual exclusion comes
            # from CreateNew, which is atomic.
            $fs.Close()
            $fs.Dispose()
            $script:LockStream = $null
            $script:LockOwned = $true
            Write-Host "[lock] adquirido: $($script:LockPath) (PID $PID)"
            return $true
        }

        $owner = Get-LockOwnerPid
        $ownerAlive = $false
        if ($owner -gt 0) {
            $ownerAlive = ($null -ne (Get-Process -Id $owner -ErrorAction SilentlyContinue))
        }
        $ageSec = 0.0
        $ageKnown = $false
        try {
            $ageSec = ((Get-Date) - (Get-Item -LiteralPath $script:LockPath -ErrorAction Stop).LastWriteTime).TotalSeconds
            $ageKnown = $true
        } catch {
        }
        # A just-created lock can be seen before its PID is written; only a
        # parseable dead PID (or a very old/long-lived file) may be removed.
        $stale = $false
        if ($owner -gt 0) {
            $stale = (-not $ownerAlive) -or ($ageKnown -and $ageSec -gt 43200)
        } elseif ($ageKnown -and $ageSec -gt 15) {
            $stale = $true
        }
        if ($stale -and (Test-Path -LiteralPath $script:LockPath)) {
            try {
                Remove-Item -LiteralPath $script:LockPath -Force -ErrorAction Stop
                Write-Host "[lock] lock obsoleto removido (PID $owner nao esta vivo ou lock antigo); tentando de novo."
                continue
            } catch {
                # Another waiter removed it first; loop.
            }
        }

        if (-not $announced) {
            if ($owner -gt 0) {
                Write-Host "[lock] outro runner ativo (PID $owner); aguardando liberar $($script:LockPath)"
            } else {
                Write-Host "[lock] lock presente mas ilegivel; aguardando liberar $($script:LockPath)"
            }
            $announced = $true
        }
        if (((Get-Date) - $start).TotalSeconds -ge $MaxWaitSec) { return $false }
        Start-Sleep -Seconds 3
    }
}

function Release-TestLock {
    if ($script:LockStream) {
        try { $script:LockStream.Close() } catch { }
        try { $script:LockStream.Dispose() } catch { }
        $script:LockStream = $null
    }
    if ($script:LockOwned) {
        try {
            Remove-Item -LiteralPath $script:LockPath -Force -ErrorAction SilentlyContinue
            Write-Host "[lock] liberado."
        } catch {
        }
        $script:LockOwned = $false
    }
}

# ---------------------------------------------------------------------------
# Foreign Godot test detection (editor and MCP processes are never touched)
# ---------------------------------------------------------------------------

function Get-ForeignGodotTests {
    param([int[]]$ExcludePid = @())
    $out = @()
    # A checagem precisa da cadeia de pais, inclusive de processos que nao sao
    # Godot (o runner e um powershell): um snapshot unico evita consultas por
    # processo e mantem a decisao coerente.
    $all = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
    $parentOf = @{}
    foreach ($q in $all) {
        try { $parentOf[[int]$q.ProcessId] = [int]$q.ParentProcessId } catch { }
    }
    # Tudo o que descende DESTE runner e arvore do proprio teste (um teste pode
    # abrir Godot filhos, ex.: determinism_cross_process_test). Nunca e "de
    # outra tarefa": nao esperar por ele nem contar como lock alheio.
    $ownRoots = New-Object System.Collections.Generic.HashSet[int]
    if ($PID -gt 0) { [void]$ownRoots.Add([int]$PID) }
    foreach ($id in @($ExcludePid)) {
        if ($null -ne $id -and [int]$id -gt 0) { [void]$ownRoots.Add([int]$id) }
    }
    foreach ($p in $all) {
        if ($p.Name -notmatch '^Godot') { continue }
        if (-not $p.CommandLine) { continue }
        if (-not ($p.CommandLine -match '--headless' -and $p.CommandLine -match '--path')) { continue }
        $procId = [int]$p.ProcessId
        if ($ExcludePid -contains $procId) { continue }
        $own = $false
        $cursor = $procId
        for ($depth = 0; $depth -lt 32; $depth++) {
            if (-not $parentOf.ContainsKey($cursor)) { break }
            $parent = [int]$parentOf[$cursor]
            if ($parent -le 0 -or $parent -eq $cursor) { break }
            if ($ownRoots.Contains($parent)) { $own = $true; break }
            $cursor = $parent
        }
        if ($own) { continue }
        $out += $p
    }
    return @($out)
}

function Wait-ForForeignGodotTests {
    param([int[]]$ExcludePid = @(), [int]$MaxWaitSec = 3600, [string]$Reason = "")
    $start = Get-Date
    $announced = $false
    while ($true) {
        $foreign = @(Get-ForeignGodotTests -ExcludePid $ExcludePid)
        if ($foreign.Count -eq 0) {
            if ($announced) { Write-Host "[lock] nenhum Godot de teste de outra tarefa restante; seguindo." }
            return $true
        }
        if (-not $announced) {
            $ids = @($foreign | ForEach-Object { $_.ProcessId }) -join ', '
            Write-Host "[lock] Godot de teste de OUTRA tarefa ativo (PID $ids); aguardando terminar ($Reason)."
            $announced = $true
        }
        if (((Get-Date) - $start).TotalSeconds -ge $MaxWaitSec) { return $false }
        Start-Sleep -Seconds 5
    }
}

# ---------------------------------------------------------------------------
# Resources (free RAM, free space on the tmp root and a C: floor warning)
# ---------------------------------------------------------------------------

function Get-FreeRamGB {
    $os = Get-CimInstance Win32_OperatingSystem
    return [math]::Round(($os.FreePhysicalMemory / 1MB), 2)
}

function Wait-ForResources {
    param([double]$MinRam, [double]$MinDisk, [int]$MaxWaitSec, [double]$MinSystemDisk = 2.0)
    $start = Get-Date
    $announced = $false
    $diskPath = $script:TmpRoot
    $diskLabel = (Get-DriveRoot $diskPath).TrimEnd('\')
    if (-not $diskLabel) { $diskLabel = $diskPath }
    while ($true) {
        $ram = Get-FreeRamGB
        $disk = Get-FreeDiskGB $diskPath
        # C: keeps its own floor, but only as a warning: a full C: is no reason
        # to abort a battery whose artifacts now live on the tmp drive.
        $system = Get-FreeDiskGB "C:\"
        if ($system -lt $MinSystemDisk -and -not $script:SystemDiskWarned) {
            Write-Host ("[recursos] aviso: C: livre {0} GB (< {1}); a rodada grava em {2}, mas C: pode acabar." -f `
                $system, $MinSystemDisk, $diskPath)
            $script:SystemDiskWarned = $true
        }
        if ($ram -ge $MinRam -and $disk -ge $MinDisk) {
            if ($announced) {
                Write-Host ("[recursos] ok: RAM livre {0} GB, {1} livre {2} GB (C: {3} GB)." -f `
                    $ram, $diskLabel, $disk, $system)
            }
            return [pscustomobject]@{ Ok = $true; Ram = $ram; Disk = $disk; System = $system }
        }
        if (-not $announced) {
            Write-Host ("[recursos] abaixo do piso: RAM livre {0} GB (< {1}) / {2} livre {3} GB (< {4}); aguardando ate {5}s." -f `
                $ram, $MinRam, $diskLabel, $disk, $MinDisk, $MaxWaitSec)
            $announced = $true
        }
        if (((Get-Date) - $start).TotalSeconds -ge $MaxWaitSec) {
            return [pscustomobject]@{ Ok = $false; Ram = $ram; Disk = $disk; System = $system }
        }
        Start-Sleep -Seconds 10
    }
}

# ---------------------------------------------------------------------------
# Process tree cleanup (only PIDs started by this runner)
# ---------------------------------------------------------------------------

function Get-ProcessSnapshot {
    return @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Select-Object ProcessId, ParentProcessId, Name, CreationDate)
}

function Get-DescendantIds {
    param($Snapshot, [int]$RootPid)
    $set = New-Object System.Collections.Generic.HashSet[int]
    [void]$set.Add($RootPid)
    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($p in $Snapshot) {
            if ($set.Contains([int]$p.ParentProcessId) -and -not $set.Contains([int]$p.ProcessId)) {
                [void]$set.Add([int]$p.ProcessId)
                $changed = $true
            }
        }
    }
    return $set
}

function Stop-ProcessTree {
    param([int]$RootPid)
    if ($RootPid -le 0) { return }
    $snapshot = Get-ProcessSnapshot
    $tree = Get-DescendantIds -Snapshot $snapshot -RootPid $RootPid
    # Include WerFault children of any node in the tree (crash dialog).
    foreach ($p in $snapshot) {
        if ($p.Name -match '^WerFault' -and $tree.Contains([int]$p.ParentProcessId)) {
            [void]$tree.Add([int]$p.ProcessId)
        }
    }

    & taskkill.exe /PID $RootPid /T /F 2>&1 | Out-Null
    Start-Sleep -Milliseconds 300

    foreach ($procId in @($tree)) {
        if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { continue }
        & taskkill.exe /PID $procId /T /F 2>&1 | Out-Null
        Start-Sleep -Milliseconds 150
        if (Get-Process -Id $procId -ErrorAction SilentlyContinue) {
            try { Stop-Process -Id $procId -Force -ErrorAction Stop } catch { }
        }
    }
}

function Clear-TestLeftovers {
    param([int]$RootPid)
    if ($RootPid -le 0) { return }
    # The root already exited; only direct children that are still alive
    # (Godot helper / WerFault dialog) are cleaned. Never touches the editor.
    $snapshot = Get-ProcessSnapshot
    foreach ($p in $snapshot) {
        if ([int]$p.ParentProcessId -ne $RootPid) { continue }
        if ($p.Name -match '^WerFault' -or $p.Name -match '^Godot') {
            & taskkill.exe /PID ([int]$p.ProcessId) /T /F 2>&1 | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# One test run
# ---------------------------------------------------------------------------

function Get-TestTimeout {
    param([string]$Name, [int]$Default, [hashtable]$Overrides)
    $keys = @($Name, ($Name -replace '_test$', ''), "$Name.gd", "$Name.tscn")
    foreach ($k in $keys) {
        if ($Overrides.ContainsKey($k)) { return [int]$Overrides[$k] }
    }
    return $Default
}

# A fresh worktree has no .godot cache: global class_name lookups and the
# GDExtension (EconomySim) only register after a first --import. Without it,
# script tests fail with parse errors and scene tests hang forever.
function Ensure-ProjectImported {
    param([int]$TimeoutSeconds)
    $marker = Join-Path $script:Root ".godot\global_script_class_cache.cfg"
    $extList = Join-Path $script:Root ".godot\extension_list.cfg"
    if ((Test-Path -LiteralPath $marker) -and (Test-Path -LiteralPath $extList)) { return "" }

    Write-Host "[import] cache do projeto ausente; rodando --import (uma vez por worktree)..."
    $outLog = Join-Path $script:LogDir "import.out.log"
    $errLog = Join-Path $script:LogDir "import.err.log"
    $godotArgs = @("--headless", "--disable-crash-handler", "--path", (Quote-Arg $script:Root), "--import")

    $proc = $null
    try {
        $proc = Start-GodotProcess -Arguments $godotArgs -OutLog $outLog -ErrLog $errLog
    } catch {
        return "import: falha ao iniciar Godot ($($_.Exception.Message))"
    }

    $exited = $proc.WaitForExit([int]($TimeoutSeconds * 1000))
    if (-not $exited) {
        Stop-ProcessTree -RootPid $proc.Id
        try { $proc.Dispose() } catch { }
        return "import: excedeu ${TimeoutSeconds}s"
    }
    try { $proc.WaitForExit() } catch { }
    $code = $null
    try { $code = $proc.ExitCode } catch { }
    if ($null -eq $code) {
        try { $proc.Refresh(); $code = $proc.ExitCode } catch { }
    }
    Clear-TestLeftovers -RootPid $proc.Id
    try { $proc.Dispose() } catch { }

    if ($null -ne $code -and $code -ne 0) {
        $tail = @(Get-LogTail -Path $errLog -Lines 15) + @(Get-LogTail -Path $outLog -Lines 15)
        if ($tail.Count -gt 0) { Write-Host ("[import] " + ($tail -join "`n         ")) }
        return ("import: exit " + (Get-ExitCodeHex -code $code))
    }
    Write-Host "[import] concluido."
    return ""
}

function Invoke-TestRun {
    param($Test, [int]$TimeoutSeconds)
    $name = $Test.Name
    $outLog = Join-Path $script:LogDir "$name.out.log"
    $errLog = Join-Path $script:LogDir "$name.err.log"

    $godotArgs = @("--headless", "--disable-crash-handler", "--path", (Quote-Arg $script:Root))
    if ($Test.Mode -eq "script") {
        $godotArgs += @("--script", (Quote-Arg $Test.ResPath))
    } elseif ($Test.Mode -eq "run") {
        $godotArgs += @("--script", (Quote-Arg $Test.ResPath))
        $runArgs = @()
        if ($Test.PSObject.Properties.Name -contains "Args") {
            foreach ($a in @($Test.Args)) { if ($null -ne $a) { $runArgs += [string]$a } }
        }
        if ($runArgs.Count -gt 0) {
            $godotArgs += @("--")
            foreach ($a in $runArgs) { $godotArgs += (Quote-Arg $a) }
        }
    } else {
        $godotArgs += @((Quote-Arg $Test.ResPath))
    }

    $record = [ordered]@{
        name = $name
        mode = $Test.Mode
        res_path = $Test.ResPath
        status = ""
        exit_code = $null
        exit_hex = ""
        duration_s = 0
        log_out = $outLog
        log_err = $errLog
        tail_out = @()
        tail_err = @()
    }

    $started = Get-Date
    $proc = $null
    try {
        $proc = Start-GodotProcess -Arguments $godotArgs -OutLog $outLog -ErrLog $errLog
    } catch {
        $record.status = "FAIL"
        $record.exit_hex = "start-error"
        $record.tail_err = @([string]$_.Exception.Message)
        $record.duration_s = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
        return $record
    }

    $ms = [int]($TimeoutSeconds * 1000)
    $exited = $proc.WaitForExit($ms)
    if (-not $exited) {
        Stop-ProcessTree -RootPid $proc.Id
        $record.status = "TIMEOUT"
        $record.exit_hex = "timeout"
        $record.duration_s = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
        $record.tail_out = Get-LogTail -Path $outLog
        $record.tail_err = Get-LogTail -Path $errLog
        try { $proc.Dispose() } catch { }
        return $record
    }

    try { $proc.WaitForExit() } catch { }
    $code = $null
    try { $code = $proc.ExitCode } catch { }
    if ($null -eq $code) {
        try { $proc.Refresh(); $code = $proc.ExitCode } catch { }
    }
    if ($null -eq $code) { $code = -1 }
    Clear-TestLeftovers -RootPid $proc.Id
    try { $proc.Dispose() } catch { }

    $record.exit_code = $code
    $record.exit_hex = Get-ExitCodeHex -code $code
    if (Test-MemoryAccessCrash -code $code) {
        $record.status = "CRASH"
    } elseif ($code -eq 0) {
        $record.status = "OK"
    } else {
        $record.status = "FAIL"
    }
    $record.duration_s = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
    if ($record.status -ne "OK") {
        $record.tail_out = Get-LogTail -Path $outLog
        $record.tail_err = Get-LogTail -Path $errLog
    }
    return $record
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

function Get-Totals {
    param($Results)
    $t = [ordered]@{ total = 0; ok = 0; fail = 0; crash = 0; timeout = 0; skipped = 0 }
    foreach ($r in $Results) {
        $t["total"] = $t["total"] + 1
        switch ($r.status) {
            "OK" { $t["ok"] = $t["ok"] + 1 }
            "FAIL" { $t["fail"] = $t["fail"] + 1 }
            "CRASH" { $t["crash"] = $t["crash"] + 1 }
            "TIMEOUT" { $t["timeout"] = $t["timeout"] + 1 }
            "SKIP" { $t["skipped"] = $t["skipped"] + 1 }
        }
    }
    return $t
}

function Write-Summary {
    param(
        [string]$RunId,
        [string]$StartedAt,
        [string]$Aborted,
        [bool]$LogsDeleted
    )
    $totals = Get-Totals -Results $script:Results
    $jsonPath = Join-Path $script:OutDir "summary.json"
    $txtPath = Join-Path $script:OutDir "summary.txt"

    # Streaming: o summary e escrito registro a registro num StreamWriter. Antes
    # um ConvertTo-Json do array inteiro + uma string unica (-join) duplicava o
    # summary na memoria e estourava (OutOfMemoryException) quando algum log
    # trazia linha gigante. O contrato de saida (campos/totais/testes) e o mesmo.
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $writer = New-Object System.IO.StreamWriter($jsonPath, $false, $utf8)
    try {
        $writer.Write("{")
        $writer.Write('"run_id":' + (ConvertTo-Json -InputObject $RunId -Compress))
        $writer.Write(',"project":' + (ConvertTo-Json -InputObject $script:Root -Compress))
        $writer.Write(',"godot":' + (ConvertTo-Json -InputObject $script:GodotExe -Compress))
        $writer.Write(',"started_at":' + (ConvertTo-Json -InputObject $StartedAt -Compress))
        $writer.Write(',"finished_at":' + (ConvertTo-Json -InputObject ((Get-Date).ToString("o")) -Compress))
        $writer.Write(',"tier":' + (ConvertTo-Json -InputObject $script:Tier -Compress))
        $writer.Write(',"base":' + (ConvertTo-Json -InputObject $script:BaseRef -Compress))
        $writer.Write(',"changed_files":' + (ConvertTo-Json -InputObject @($script:ChangedFiles) -Compress -Depth 4))
        $writer.Write(',"selection":' + (ConvertTo-Json -InputObject $script:SelectionReasons -Compress -Depth 5))
        $writer.Write(',"aborted":' + (ConvertTo-Json -InputObject $Aborted -Compress))
        $writer.Write(',"logs_deleted":' + (ConvertTo-Json -InputObject $LogsDeleted -Compress))
        $writer.Write(',"totals":' + (ConvertTo-Json -InputObject $totals -Compress -Depth 4))
        $writer.Write(',"tests":[')
        $first = $true
        foreach ($r in $script:Results) {
            if (-not $first) { $writer.Write(",") }
            $first = $false
            $writer.Write((ConvertTo-Json -InputObject $r -Compress -Depth 4))
        }
        $writer.Write("]}")
    } finally {
        $writer.Dispose()
    }

    $txt = New-Object System.IO.StreamWriter($txtPath, $false, $utf8)
    try {
        $txt.WriteLine("OpenGS test run $RunId")
        $txt.WriteLine("project: $($script:Root)")
        $txt.WriteLine("godot:   $($script:GodotExe)")
        $txt.WriteLine("total=$($totals['total']) ok=$($totals['ok']) fail=$($totals['fail']) crash=$($totals['crash']) timeout=$($totals['timeout']) skipped=$($totals['skipped'])")
        if ($Aborted) { $txt.WriteLine("ABORTED: $Aborted") }
        foreach ($r in $script:Results) {
            $suffix = ""
            if ($r.exit_hex) { $suffix = " exit=$($r.exit_hex)" }
            $txt.WriteLine(("[{0}] {1} ({2}){3} {4}s" -f $r.status, $r.name, $r.mode, $suffix, $r.duration_s))
            if ($r.status -eq "SKIP" -and $r.tail_err.Count -gt 0) {
                $txt.WriteLine("        " + ($r.tail_err -join " "))
            }
        }
    } finally {
        $txt.Dispose()
    }

    return [pscustomobject]@{ Totals = $totals; JsonPath = $jsonPath; TxtPath = $txtPath }
}

# ---------------------------------------------------------------------------
# Main flow
# ---------------------------------------------------------------------------

function Invoke-Main {
    $root = $ProjectRoot
    if (-not $root) { $root = Split-Path -Parent $PSScriptRoot }
    $root = (Resolve-Path -LiteralPath $root).Path
    if (-not (Test-Path -LiteralPath (Join-Path $root "project.godot"))) {
        throw "project.godot nao encontrado em $root"
    }
    $script:Root = $root
    $script:GodotExe = Find-GodotExe -Explicit $Godot
    if ($Parallel -lt 1) { throw "-Parallel precisa ser >= 1" }
    if ($Tier -and (@("quick", "pr", "nightly") -notcontains $Tier.ToLower())) {
        throw "-Tier aceita quick, pr ou nightly (recebido: '$Tier')"
    }

    $script:TmpRoot = Resolve-TmpRoot -Requested $TmpRoot
    $script:GodotTmpDir = Join-Path $script:TmpRoot "godot_tmp"
    if (-not (Test-Path -LiteralPath $script:GodotTmpDir)) {
        New-Item -ItemType Directory -Path $script:GodotTmpDir -Force | Out-Null
    }
    $script:LockPath = Join-Path $script:TmpRoot "opengs_godot_test.lock"

    $runId = (Get-Date).ToString("yyyyMMdd_HHmmss")
    if ($OutDir) {
        $script:OutDir = $OutDir
    } else {
        $script:OutDir = Join-Path $script:TmpRoot ("opengs_test_run_" + $runId)
    }
    if (-not (Test-Path -LiteralPath $script:OutDir)) {
        New-Item -ItemType Directory -Path $script:OutDir -Force | Out-Null
    }
    $script:LogDir = Join-Path $script:OutDir "logs"
    if (-not (Test-Path -LiteralPath $script:LogDir)) {
        New-Item -ItemType Directory -Path $script:LogDir -Force | Out-Null
    }

    # `powershell -File` delivers "-List a,b" as a single string, so split commas here.
    $listItems = @()
    foreach ($raw in $List) {
        if (-not $raw) { continue }
        foreach ($part in ($raw -split ',')) {
            if ($part.Trim()) { $listItems += $part.Trim() }
        }
    }
    $overrideItems = @()
    foreach ($raw in $TimeoutOverrides) {
        if (-not $raw) { continue }
        foreach ($part in ($raw -split ',')) {
            if ($part.Trim()) { $overrideItems += $part.Trim() }
        }
    }

    $catalog = Get-TestCatalog -Root $root
    $tierMode = ([string]$Tier).ToLower()
    $selected = @()
    $runs = @()

    if ($tierMode) {
        $mapPath = $ImpactMap
        if (-not $mapPath) { $mapPath = Join-Path $root "tools/test_impact_map.json" }
        $script:ImpactMapPath = (Resolve-Path -LiteralPath $mapPath).Path
        $map = Read-ImpactMap -Path $script:ImpactMapPath
        $script:Tier = $tierMode
        $script:BaseRef = $Base
        $script:ChangedFiles = @(Get-ChangedFiles -Root $root -BaseRef $Base)

        $impact = Get-ImpactSelection -Map $map -Changed $script:ChangedFiles
        $script:SelectionReasons = $impact.Reasons
        $script:SelectionNeedsNightly = $impact.NeedsNightly
        $script:SelectionCoreTouched = $impact.CoreTouched

        $impactNames = @($impact.Names)
        $allRequested = ($tierMode -eq "nightly") -or ($impactNames -contains "__ALL__") -or $All
        if ($allRequested) {
            $selected = @($catalog | Where-Object { $_.Mode -ne "skip" })
        } else {
            $fromExplicit = $false
            if ($listItems.Count -gt 0 -or $Pattern) {
                $selected = @(Select-Tests -Catalog $catalog -Names $listItems -NamePattern $Pattern)
                $fromExplicit = $true
            } else {
                $byName = @{}
                foreach ($c in $catalog) { $byName[$c.Name] = $c }
                $merged = @()
                $missing = @()
                foreach ($n in $impactNames) {
                    if ($n -eq "ALL" -or $n -eq "__ALL__") { continue }
                    if ($byName.ContainsKey($n)) { $merged += $byName[$n] } else { $missing += $n }
                }
                if ($missing.Count -gt 0) {
                    Write-Host ("[tier] aviso: {0} teste(s) do mapa fora do catalogo: {1}" -f $missing.Count, ($missing -join ", "))
                }
                $selected = $merged
            }

            # Cheap sentinels run in quick AND pr (always), on top of the impact list.
            $byName = @{}
            foreach ($c in $catalog) { $byName[$c.Name] = $c }
            $withSentinels = @()
            foreach ($s in @($map.sentinels.cheap)) {
                if (-not $s) { continue }
                if ($byName.ContainsKey([string]$s)) {
                    $withSentinels += $byName[[string]$s]
                } else {
                    Write-Host "[tier] aviso: sentinela '$s' fora do catalogo"
                }
            }
            foreach ($t in $selected) {
                $dup = $false
                foreach ($w in $withSentinels) { if ($w.Name -eq $t.Name) { $dup = $true; break } }
                if (-not $dup) { $withSentinels += $t }
            }
            $selected = $withSentinels
            if ($fromExplicit) {
                Write-Host "[tier] -List/-Pattern informado: selecao explicita + sentinelas."
            }
        }

        $rank = Get-TierRank $tierMode
        foreach ($r in @($map.sentinels.runs)) {
            if (-not $r) { continue }
            if ((Get-TierRank ([string]$r.tier)) -gt $rank) { continue }
            $runTimeout = $TimeoutSec
            if ($r.timeout_s) { $runTimeout = [int]$r.timeout_s }
            $runs += [pscustomobject]@{
                Name = [string]$r.name
                Mode = "run"
                ResPath = [string]$r.script
                SkipReason = ""
                Args = @($r.args)
                TimeoutSec = $runTimeout
            }
        }

        $selected = @($selected | Sort-Object Name -Unique)
        if ($runs.Count -gt 0) { $selected += $runs }
        if ($tierMode -ne "nightly") {
            if ($script:SelectionCoreTouched) {
                # File stays ASCII-only: build the accented warning from code points.
                $coreMsg = "mudan" + [char]0x00E7 + "a no n" + [char]0x00FA + "cleo: rode -Tier nightly antes do merge se poss" + [char]0x00ED + "vel"
                Write-Host ("[tier] aviso: " + $coreMsg)
            } elseif ($script:SelectionNeedsNightly) {
                Write-Host "[tier] aviso: caminho central alterado; a cobertura completa pede -Tier nightly."
            }
        }
    } else {
        $selected = @(Select-Tests -Catalog $catalog -Names $listItems -NamePattern $Pattern)
    }
    if ($selected.Count -eq 0) { throw "nenhum teste selecionado" }

    $overrides = @{}
    foreach ($entry in $overrideItems) {
        if (-not $entry) { continue }
        if ($entry -match '^\s*([^=]+?)\s*=\s*(\d+)\s*$') {
            $overrides[$Matches[1]] = [int]$Matches[2]
        } else {
            Write-Host "[runner] override de timeout ignorado (formato nome=segundos): $entry"
        }
    }

    Write-Host "[runner] godot:   $($script:GodotExe)"
    Write-Host "[runner] projeto: $root"
    Write-Host "[runner] tmp:     $($script:TmpRoot) (TEMP/TMP do Godot: $($script:GodotTmpDir))"
    Write-Host "[runner] testes:  $($selected.Count) (timeout padrao ${TimeoutSec}s)"
    Write-Host "[runner] saida:   $($script:OutDir)"
    if ($tierMode) {
        Write-Host ("[tier]     {0} (base {1}; {2} arquivo(s) alterado(s))" -f `
            $tierMode, $Base, $script:ChangedFiles.Count)
        foreach ($f in $script:ChangedFiles) { Write-Host "             diff: $f" }
        foreach ($k in $script:SelectionReasons.Keys) {
            $why = (@($script:SelectionReasons[$k]) -join ", ")
            Write-Host ("             {0} <- {1}" -f $k, $why)
        }
    }

    if ($DryRun) {
        # CLI checks (sem Godot) que rodariam antes do lock: fail-fast primeiro,
        # ou seja, `sim_unit_tests` antes de qualquer outra checagem de CLI.
        $cliDry = @()
        if ($tierMode) {
            $cliDry = @(Get-CliChecks -Map $map -TierName $tierMode)
        }
        if ($cliDry.Count -gt 0) {
            $cliFast = @($cliDry | Where-Object { $_.fail_fast })
            $cliRest = @($cliDry | Where-Object { -not $_.fail_fast })
            $cliDry = @($cliFast + $cliRest)
            Write-Host ("[dry-run] {0} checagem(ns) de CLI (antes do lock, sem Godot):" -f $cliDry.Count)
            foreach ($c in $cliDry) {
                $cmd = [string]$c.command
                $argText = (@($c.args) -join " ")
                if ($argText) { $cmd = "$cmd $argText" }
                $tag = ""
                if ($c.fail_fast) { $tag = " fail-fast" }
                $missing = @()
                foreach ($req in @($c.requires)) {
                    if (-not $req) { continue }
                    if (-not (Test-Path -LiteralPath (Join-Path $root ([string]$req)))) {
                        $missing += [string]$req
                    }
                }
                if ($missing.Count -gt 0) {
                    $tag += " [SKIP: falta " + ($missing -join ", ") + "]"
                }
                Write-Host ("  cli     {0}{1} ({2})" -f $c.name, $tag, $cmd)
            }
        }
        Write-Host ("[dry-run] {0} entrada(s); nenhum Godot sera aberto." -f $selected.Count)
        foreach ($t in $selected) {
            if ($t.Mode -eq "run") {
                Write-Host ("  run     {0} ({1})" -f $t.Name, (@($t.Args) -join " "))
            } else {
                Write-Host ("  {0,-7} {1}" -f $t.Mode, $t.Name)
            }
        }
        $selectionPath = Join-Path $script:OutDir "selection.json"
        $payload = [ordered]@{
            tier = $tierMode
            base = $Base
            changed_files = @($script:ChangedFiles)
            selected = @($selected | ForEach-Object { $_.Name })
            cli_checks = @($cliDry | ForEach-Object { [string]$_.name })
            reasons = $script:SelectionReasons
            needs_nightly = $script:SelectionNeedsNightly
            core_touched = $script:SelectionCoreTouched
        }
        Write-Utf8NoBom -Path $selectionPath -Text (ConvertTo-Json -InputObject $payload -Depth 6)
        Write-Host "[dry-run] selecao: $selectionPath"
        return 0
    }

    # CLI checks never start Godot, so they run before the global lock and are
    # the only part that uses -Parallel.
    $startedAt = (Get-Date).ToString("o")
    $aborted = ""
    $setupCode = 0
    if ($tierMode) {
        $cliChecks = @(Get-CliChecks -Map $map -TierName $tierMode)
        if ($cliChecks.Count -gt 0) {
            $cliRecords = @(Invoke-CliChecks -Checks $cliChecks -MaxParallel $Parallel -Root $root)
            $script:Results += $cliRecords
            # `fail_fast`: uma checagem barata (ex.: sim_unit_tests) derruba o
            # tier antes de qualquer Godot ser aberto.
            $failFastNames = @($cliChecks | Where-Object { $_.fail_fast } |
                ForEach-Object { [string]$_.name })
            $failedFast = @($cliRecords | Where-Object {
                ($_.status -eq "FAIL" -or $_.status -eq "TIMEOUT") -and
                ($failFastNames -contains [string]$_.name)
            })
            if ($failedFast.Count -gt 0) {
                $names = (@($failedFast | ForEach-Object { [string]$_.name }) -join ", ")
                $aborted = "cli fail-fast: $names falhou"
                Write-Host "[cli] $aborted. Abortando antes de qualquer teste Godot."
                $setupCode = 1
            }
        }
    }

    if ($setupCode -eq 0 -and -not (Acquire-TestLock -MaxWaitSec $LockWaitSec)) {
        Write-Host "[lock] nao consegui o lock em ${LockWaitSec}s. Abortando sem rodar teste."
        $aborted = "lock: outro runner segurou $($script:LockPath) por mais de ${LockWaitSec}s"
        $setupCode = 3
    }

    if ($setupCode -eq 0 -and -not (Wait-ForForeignGodotTests -MaxWaitSec $LockWaitSec -Reason "antes de comecar")) {
        Write-Host "[lock] ainda ha Godot de teste de outra tarefa. Abortando sem rodar teste."
        $aborted = "lock: Godot de teste estranho ativo por mais de ${LockWaitSec}s"
        $setupCode = 3
    }

    if ($setupCode -eq 0) {
        $importError = Ensure-ProjectImported -TimeoutSeconds $ImportTimeoutSec
        if ($importError) {
            Write-Host "[import] $importError. Abortando a bateria."
            $aborted = $importError
            $setupCode = 2
        }
    }

    if ($setupCode -eq 0) {
        foreach ($test in $selected) {
            if ($test.Mode -eq "skip") {
                $script:Results += [ordered]@{
                    name = $test.Name
                    mode = "skip"
                    res_path = ""
                    status = "SKIP"
                    exit_code = $null
                    exit_hex = ""
                    duration_s = 0
                    log_out = ""
                    log_err = ""
                    tail_out = @()
                    tail_err = @($test.SkipReason)
                }
                Write-Host "[skip]  $($test.Name): $($test.SkipReason)"
                continue
            }

            if (-not (Wait-ForForeignGodotTests -MaxWaitSec $LockWaitSec -Reason "antes de $($test.Name)")) {
                $aborted = "lock: Godot de teste estranho ativo por mais de ${LockWaitSec}s antes de $($test.Name)"
                $setupCode = 3
                break
            }

            $res = Wait-ForResources -MinRam $MinFreeRamGB -MinDisk $MinFreeDiskGB -MaxWaitSec $ResourceWaitSec
            if (-not $res.Ok) {
                $aborted = ("recursos: RAM livre {0} GB (< {1}) / disco de {2} livre {3} GB (< {4}) apos {5}s" -f `
                    $res.Ram, $MinFreeRamGB, $script:TmpRoot, $res.Disk, $MinFreeDiskGB, $ResourceWaitSec)
                Write-Host "[recursos] $aborted. Abortando a bateria."
                $setupCode = 4
                break
            }

            $timeout = $TimeoutSec
            if (($test.PSObject.Properties.Name -contains "TimeoutSec") -and $test.TimeoutSec) {
                $timeout = [int]$test.TimeoutSec
            } else {
                $timeout = Get-TestTimeout -Name $test.Name -Default $TimeoutSec -Overrides $overrides
            }
            $record = Invoke-TestRun -Test $test -TimeoutSeconds $timeout
            $script:Results += $record

            $suffix = ""
            if ($record.exit_hex) { $suffix = " exit=$($record.exit_hex)" }
            Write-Host ("[{0,-7}] {1} ({2}){3} {4}s" -f $record.status.ToLower(), $record.name, $record.mode, $suffix, $record.duration_s)
        }
    }

    # Delete per-test logs only when everything is green (summary is kept).
    $totals = Get-Totals -Results $script:Results
    $bad = $totals["fail"] + $totals["crash"] + $totals["timeout"]
    $logsDeleted = $false
    if (-not $KeepLogs -and $setupCode -eq 0 -and $bad -eq 0 -and $totals["skipped"] -eq 0) {
        try {
            if (Test-Path -LiteralPath $script:LogDir) {
                Remove-Item -LiteralPath $script:LogDir -Recurse -Force -ErrorAction Stop
            }
            $logsDeleted = $true
        } catch {
            Write-Host "[runner] aviso: nao consegui apagar a pasta de logs: $($_.Exception.Message)"
        }
    }

    $summary = Write-Summary -RunId $runId -StartedAt $startedAt -Aborted $aborted -LogsDeleted $logsDeleted
    Write-Host ""
    Write-Host ("[resumo] total={0} ok={1} falha={2} crash={3} timeout={4} pulado={5}" -f `
        $summary.Totals["total"], $summary.Totals["ok"], $summary.Totals["fail"], `
        $summary.Totals["crash"], $summary.Totals["timeout"], $summary.Totals["skipped"])
    foreach ($r in $script:Results) {
        if ($r.status -eq "OK") { continue }
        $extra = ""
        if ($r.exit_hex) { $extra = " exit=$($r.exit_hex)" }
        Write-Host ("  {0}: {1}{2}" -f $r.status, $r.name, $extra)
    }
    Write-Host "[resumo] JSON: $($summary.JsonPath)"
    Write-Host "[resumo] TXT:  $($summary.TxtPath)"
    if ($logsDeleted) { Write-Host "[resumo] logs apagados (tudo verde)." }

    if ($setupCode -ne 0) { return $setupCode }
    if ($bad -gt 0) { return 1 }
    return 0
}

$finalCode = 0
try {
    $finalCode = Invoke-Main
} catch {
    Write-Host ("[erro] " + $_.Exception.Message)
    $finalCode = 2
} finally {
    Release-TestLock
}
exit $finalCode
