# run_tests.ps1 -- canonical headless test runner for OpenGS Game.
#
# WHY: test tasks used to open several Godot processes at once, some got stuck
# on the crash/ALERT dialog after `--script` was pointed at a scene, and memory
# was not coming back. This script is the ONLY supported way to run tests:
#   - one Godot test process on the whole machine (global lock + foreign check)
#   - free RAM/disk floor checked before every test
#   - `--script` only for SceneTree/MainLoop scripts; scenes run as scenes
#   - hard timeout, process-tree kill, WerFault cleanup, exit-code recording
#   - JSON + per-test logs in a private temp folder
#
# The file is intentionally ASCII-only: accented Windows paths (OneDrive
# "Area de Trabalho") must not break parsing/encoding on PowerShell 5.1.
#
# Usage (PowerShell 5.1+):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -All
#   ... -List services_test,economy_integration_test
#   ... -Pattern "*economy*" -TimeoutSec 1200
#   ... -TimeoutOverrides "economy_long_run_test=3600,world_equivalence_test=1800"
#
# Exit codes:
#   0 all green
#   1 at least one FAIL / CRASH / TIMEOUT
#   2 setup error (Godot not found, unknown selection, nothing to run)
#   3 lock wait exceeded LockWaitSec
#   4 resource floor not reached within ResourceWaitSec
#
# Never run this in parallel with itself or with another Godot test process.
# The lock file is %TEMP%\opengs_godot_test.lock (PID inside) and is always
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
    [string]$OutDir = "",
    [switch]$KeepLogs
)

$ErrorActionPreference = "Stop"

$script:Root = ""
$script:GodotExe = ""
$script:LockPath = Join-Path $env:TEMP "opengs_godot_test.lock"
$script:LockStream = $null
$script:LockOwned = $false
$script:OutDir = ""
$script:LogDir = ""
$script:Results = @()

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
    $procs = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'Godot%'" -ErrorAction SilentlyContinue)
    foreach ($p in $procs) {
        if (-not $p.CommandLine) { continue }
        if ($ExcludePid -contains [int]$p.ProcessId) { continue }
        if ($p.CommandLine -match '--headless' -and $p.CommandLine -match '--path') { $out += $p }
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
# Resources (free RAM and C: free space)
# ---------------------------------------------------------------------------

function Get-FreeRamGB {
    $os = Get-CimInstance Win32_OperatingSystem
    return [math]::Round(($os.FreePhysicalMemory / 1MB), 2)
}

function Get-FreeSystemDriveGB {
    try {
        $d = Get-PSDrive -Name C -ErrorAction Stop
        return [math]::Round(($d.Free / 1GB), 2)
    } catch {
        $ld = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='C:'" -ErrorAction SilentlyContinue
        if ($ld) { return [math]::Round(($ld.FreeSpace / 1GB), 2) }
        return 0.0
    }
}

function Wait-ForResources {
    param([double]$MinRam, [double]$MinDisk, [int]$MaxWaitSec)
    $start = Get-Date
    $announced = $false
    while ($true) {
        $ram = Get-FreeRamGB
        $disk = Get-FreeSystemDriveGB
        if ($ram -ge $MinRam -and $disk -ge $MinDisk) {
            if ($announced) { Write-Host ("[recursos] ok: RAM livre {0} GB, C: livre {1} GB." -f $ram, $disk) }
            return [pscustomobject]@{ Ok = $true; Ram = $ram; Disk = $disk }
        }
        if (-not $announced) {
            Write-Host ("[recursos] abaixo do piso: RAM livre {0} GB (< {1}) / C: livre {2} GB (< {3}); aguardando ate {4}s." -f $ram, $MinRam, $disk, $MinDisk, $MaxWaitSec)
            $announced = $true
        }
        if (((Get-Date) - $start).TotalSeconds -ge $MaxWaitSec) {
            return [pscustomobject]@{ Ok = $false; Ram = $ram; Disk = $disk }
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
        $proc = Start-Process -FilePath $script:GodotExe -ArgumentList $godotArgs `
            -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
            -NoNewWindow -PassThru -ErrorAction Stop
        $null = $proc.Handle
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
        $proc = Start-Process -FilePath $script:GodotExe -ArgumentList $godotArgs `
            -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
            -NoNewWindow -PassThru -ErrorAction Stop
        # Keep the OS handle: without it PowerShell 5.1 can report ExitCode as
        # $null for a redirected process that exits quickly.
        $null = $proc.Handle
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

    $runId = (Get-Date).ToString("yyyyMMdd_HHmmss")
    if ($OutDir) {
        $script:OutDir = $OutDir
    } else {
        $script:OutDir = Join-Path $env:TEMP ("opengs_test_run_" + $runId)
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
    $selected = @(Select-Tests -Catalog $catalog -Names $listItems -NamePattern $Pattern)
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
    Write-Host "[runner] testes:  $($selected.Count) (timeout padrao ${TimeoutSec}s)"
    Write-Host "[runner] saida:   $($script:OutDir)"

    $startedAt = (Get-Date).ToString("o")
    $aborted = ""
    $setupCode = 0

    if (-not (Acquire-TestLock -MaxWaitSec $LockWaitSec)) {
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
                $aborted = ("recursos: RAM livre {0} GB (< {1}) / C: livre {2} GB (< {3}) apos {4}s" -f `
                    $res.Ram, $MinFreeRamGB, $res.Disk, $MinFreeDiskGB, $ResourceWaitSec)
                Write-Host "[recursos] $aborted. Abortando a bateria."
                $setupCode = 4
                break
            }

            $timeout = Get-TestTimeout -Name $test.Name -Default $TimeoutSec -Overrides $overrides
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
