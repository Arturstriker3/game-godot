# Estresse de encerramento dos testes de economia.
#
# Roda o MESMO teste N vezes seguidas, cada execução num processo novo, com
# timeout por execução, e conta códigos de saída != 0 (0xC0000005 =
# -1073741819 = access violation no Windows).
#
# Uso:
#   powershell -File tools/stress_shutdown.ps1 -Script tools/economy_ai_test.gd -Runs 30
#   powershell -File tools/stress_shutdown.ps1 -Script tools/convoy_lifecycle_test.tscn -Scene
#
# Saída: uma linha por execução e um resumo final "RESUMO ... falhas=N".

param(
    [Parameter(Mandatory = $true)][string]$Script,
    [int]$Runs = 30,
    [int]$TimeoutSec = 300,
    [switch]$Scene,
    [string]$Tag = "run"
)

$ErrorActionPreference = "Stop"
$godot = Join-Path ([Environment]::GetFolderPath("Desktop")) "Godot_v4.7.2-stable_win64.exe"
$project = Split-Path -Parent $PSScriptRoot
$work = Join-Path $env:TEMP "ogs_stress_$Tag"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$crashes = 0
$timeouts = 0
$other = 0
$durations = @()

for ($i = 1; $i -le $Runs; $i++) {
    $out = Join-Path $work ("out_$i.txt")
    $err = Join-Path $work ("err_$i.txt")
    $gargs = @("--headless", "--path", $project)
    if ($Scene) {
        $gargs += "res://$Script"
    } else {
        $gargs += @("--script", "res://$Script")
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $godot
    $psi.Arguments = ($gargs | ForEach-Object { if ($_ -match ' ') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.WorkingDirectory = $project
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc.Start() | Out-Null
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill() } catch {}
        $proc.WaitForExit()
        $timeouts++
        $sw.Stop()
        Write-Output ("[{0}] #{1}: TIMEOUT ({2}s)" -f $Tag, $i, $TimeoutSec)
        continue
    }
    $proc.WaitForExit()
    $sw.Stop()
    [IO.File]::WriteAllText($out, $outTask.Result)
    [IO.File]::WriteAllText($err, $errTask.Result)
    $durations += $sw.Elapsed.TotalMilliseconds
    $code = $proc.ExitCode
    $ok = $false
    if (Test-Path $out) {
        $text = Get-Content -Raw $out
        $ok = $text -match " OK"
    }
    if ($code -ne 0) {
        if ($code -eq -1073741819) {
            $crashes++
            Write-Output ("[{0}] #{1}: ACCESS VIOLATION 0xC0000005 (OK impresso={2})" -f $Tag, $i, $ok)
        } else {
            $other++
            Write-Output ("[{0}] #{1}: exit={2} 0x{3:X8} (OK impresso={4})" -f $Tag, $i, $code, ($code -band 0xFFFFFFFF), $ok)
        }
    } elseif (-not $ok) {
        Write-Output ("[{0}] #{1}: exit=0 mas SEM 'OK' no log" -f $Tag, $i)
    }
}

$avg = if ($durations.Count -gt 0) { [math]::Round(($durations | Measure-Object -Average).Average) } else { 0 }
Write-Output ("RESUMO {0}: runs={1} access_violation={2} timeout={3} outro_exit!=0={4} media_ms={5} logs={6}" -f `
    $Tag, $Runs, $crashes, $timeouts, $other, $avg, $work)
if (($crashes + $timeouts + $other) -gt 0) { exit 1 }
exit 0
