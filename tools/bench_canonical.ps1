# Bench canônico A0 — docs/plans/perf_pop_financas_plano.md, seção A0.
#
# Roda N rodadas sequenciais de `tools/perf_day_peak.tscn` em Release headless
# (tick no worker, --profile=1), cada uma num processo novo, e grava o JSON de
# baseline versionado em docs/economy/perf/baseline_A0.json.
#
# Como o binário estável do Godot carrega a DLL do slot *debug*, o release de
# perfil (`gdextension/build-profile-release`, -DOGS_PROFILE_RELEASE=ON) é
# copiado temporariamente por cima do slot antes da troca de rodadas e SEMPRE
# restaurado no finally (hash SHA-256 conferido antes e depois).
#
# Uso:
#   powershell -NoProfile -File tools/bench_canonical.ps1
#   powershell -NoProfile -File tools/bench_canonical.ps1 -Days 30 -Rounds 1 `
#       -Out docs/economy/perf/scratch.json
#
# Códigos de saída:
#   0  bench concluído — o veredito do critério de 3% está em `criterion.ok` e
#      na última linha impressa (reprovar o critério não é erro de execução)
#   2  pré-requisito ausente (editor Godot aberto, Godot/DLL inexistente) ou
#      rodada falhou/estourou o tempo limite
#   3  a DLL debug NÃO voltou ao slot — restaure o backup impresso à mão

param(
    [int]$Days = 60,
    [int]$Rounds = 3,
    [string]$Godot = "",
    [string]$Out = "docs/economy/perf/baseline_A0.json",
    [string]$ReleaseDll = "gdextension/bin/economy_gdextension.windows.template_release.x86_64.dll",
    [string]$Slot = "gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll",
    [int]$RoundTimeoutMin = 90
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
# Aceite A0: variação entre rodadas <= 3%. O critério vale para as métricas-chefe
# (tick e dia completo); as demais são publicadas e comparadas por referência.
# Métricas de ruído (poll ~0 ms, fases que zeram no mapa=none) não podem reprovar
# o bench: a variação relativa delas estoura com qualquer jitter de 1 µs.
$criterion = 3.0
$criterionScope = @("tick_wall_ms", "day_total_ms")
# Builds necessários para gerar as duas DLLs (usado nas mensagens de aborto).
$cmake = "G:/tools/cpp-tools/Lib/site-packages/cmake/data/bin/cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = "cmake" }

# Caminho relativo à raiz do projeto (o script pode ser chamado de qualquer cwd).
function Resolve-FromRoot([string]$path) {
    if ([System.IO.Path]::IsPathRooted($path)) { return $path }
    return (Join-Path $root $path)
}

function Get-Sha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Mediana clássica (média dos dois centrais quando N é par).
function Get-Median([double[]]$values) {
    if ($values.Count -eq 0) { return 0.0 }
    $sorted = @($values | Sort-Object)
    $n = $sorted.Count
    $mid = [int][math]::Floor($n / 2)
    if ($n % 2 -eq 1) { return [double]$sorted[$mid] }
    return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}

# Start-Process recebe uma única string de argumentos: cita o que tem espaço.
function Quote-Arg([string]$value) {
    if ($value -match '[\s"]') { return '"' + $value + '"' }
    return $value
}

# ---------------------------------------------------------------------------
# 1. Pré-requisitos: editor fechado, Godot e as duas DLLs no lugar.
# ---------------------------------------------------------------------------
# Qualquer Godot com janela (editor) invalida a medição — e nunca é morto por
# este script, que só encerra os próprios processos filhos.
$editors = @()
try {
    $editors = @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like "Godot*" -and $_.MainWindowHandle -ne 0 })
} catch { }
if ($editors.Count -gt 0) {
    Write-Host ""
    Write-Host ("ABORTADO: há editor Godot aberto (PID {0})." -f (($editors | ForEach-Object { $_.Id }) -join ", ")) -ForegroundColor Red
    Write-Host "Feche o editor e rode de novo: o bench troca a DLL no disco." -ForegroundColor Red
    exit 2
}
# Outro Godot sem janela (bench/sweep headless de outra frente) rouba CPU e
# invalida a medição. Não é fatal para o script, mas o número não vale.
$others = @()
try {
    $others = @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like "Godot*" -and $_.MainWindowHandle -eq 0 })
} catch { }
if ($others.Count -gt 0) {
    Write-Host ("[a0] AVISO: outro processo Godot headless ativo (PID {0}) — a medição pode sair ruidosa." -f (($others | ForEach-Object { $_.Id }) -join ", ")) -ForegroundColor Yellow
}

if ([string]::IsNullOrWhiteSpace($Godot)) {
    $Godot = Join-Path ([Environment]::GetFolderPath("Desktop")) "Godot_v4.7.2-stable_win64.exe"
}
$godotExe = Resolve-FromRoot $Godot
if (-not (Test-Path -LiteralPath $godotExe)) {
    Write-Host ""
    Write-Host "ABORTADO: executável do Godot não encontrado: $godotExe" -ForegroundColor Red
    Write-Host "Passe o caminho com -Godot <path>." -ForegroundColor Red
    exit 2
}

$releasePath = Resolve-FromRoot $ReleaseDll
$slotPath = Resolve-FromRoot $Slot
$missing = @()
if (-not (Test-Path -LiteralPath $releasePath)) { $missing += $releasePath }
if (-not (Test-Path -LiteralPath $slotPath)) { $missing += $slotPath }
if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "ABORTADO: DLL ausente:" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host "  $m" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Build necessário (na raiz $root):"
    Write-Host ("  & `"{0}`" -S gdextension -B gdextension/build-profile-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DOGS_PROFILE_RELEASE=ON" -f $cmake)
    Write-Host ("  & `"{0}`" --build gdextension/build-profile-release" -f $cmake)
    Write-Host ("  & `"{0}`" -S gdextension -B gdextension/build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug" -f $cmake)
    Write-Host ("  & `"{0}`" --build gdextension/build-debug" -f $cmake)
    exit 2
}
# Slot e release iguais destruiriam a DLL debug no backup (nada a restaurar).
if ((Resolve-Path -LiteralPath $slotPath).Path -eq (Resolve-Path -LiteralPath $releasePath).Path) {
    Write-Host ""
    Write-Host "ABORTADO: -Slot e -ReleaseDll apontam para o mesmo arquivo ($slotPath)." -ForegroundColor Red
    exit 2
}

$outPath = Resolve-FromRoot $Out
$outDir = Split-Path -Parent $outPath
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

# Logs e backup das DLLs vivem em %TEMP% (nada é apagado no fim).
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("opengs_a0\" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$backupPath = Join-Path $work "debug_slot.dll"

# Host: só para o baseline ser interpretável (nenhum número é inventado).
$cpu = "desconhecido"
$osCaption = "desconhecido"
try { $cpu = (Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1 -ExpandProperty Name).Trim() } catch { }
try { $osCaption = (Get-CimInstance -ClassName Win32_OperatingSystem).Caption } catch { }

Write-Host ("[a0] raiz   {0}" -f $root)
Write-Host ("[a0] godot  {0}" -f $godotExe)
Write-Host ("[a0] slot   {0}" -f $slotPath)
Write-Host ("[a0] release {0}" -f $releasePath)
Write-Host ("[a0] saida  {0}" -f $outPath)
Write-Host ("[a0] logs   {0}" -f $work)

$exitCode = 0
$restoreOk = $true
$swapped = $false
$slotShaBefore = $null
$slotShaAfter = $null
$releaseSha = $null
$roundResults = @()

try {
    # -----------------------------------------------------------------------
    # 2. Troca das DLLs: hash do slot, backup, release por cima, hash conferido.
    # -----------------------------------------------------------------------
    $slotShaBefore = Get-Sha256 $slotPath
    $releaseSha = Get-Sha256 $releasePath
    Write-Host ("[a0] sha do slot (antes) {0}" -f $slotShaBefore)

    Copy-Item -LiteralPath $slotPath -Destination $backupPath -Force
    # `$swapped` liga a restauração do finally ANTES de tocar no slot: se a
    # cópia do release falhar no meio, o finally ainda devolve o backup.
    $swapped = $true
    Copy-Item -LiteralPath $releasePath -Destination $slotPath -Force
    $slotShaAfter = Get-Sha256 $slotPath
    if ($slotShaAfter -ne $releaseSha) {
        throw "cópia do release sobre o slot não conferiu (sha $slotShaAfter != $releaseSha)"
    }
    Write-Host ("[a0] release copiado sobre o slot (sha {0})" -f $slotShaAfter)

    # -----------------------------------------------------------------------
    # 3. Rodadas sequenciais.
    # -----------------------------------------------------------------------
    $timeoutMs = $RoundTimeoutMin * 60 * 1000
    for ($r = 1; $r -le $Rounds; $r++) {
        $roundJson = Join-Path $work ("round_$r.json")
        $stdout = Join-Path $work ("round_$r.out.txt")
        $stderr = Join-Path $work ("round_$r.err.txt")
        $log = Join-Path $work ("round_$r.log")

        # Threads NÃO são passadas: o núcleo fica no padrão dele.
        $gargs = @(
            "--headless", "--path", $root, "res://tools/perf_day_peak.tscn", "--",
            "--days=$Days", "--tick-mode=worker", "--profile=1", "--json=$roundJson"
        )
        Write-Host ("[a0] rodada {0}/{1}: {2} dias, tick=worker, profile=1" -f $r, $Rounds, $Days)

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $proc = Start-Process -FilePath $godotExe `
            -ArgumentList (($gargs | ForEach-Object { Quote-Arg $_ }) -join " ") `
            -WorkingDirectory $root -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (-not $proc.WaitForExit($timeoutMs)) {
            # Timeout: encerra SÓ o PID filho desta rodada (nunca terceiros)
            # e deixa o finally restaurar a DLL debug.
            try { $proc.Kill() } catch { }
            $proc.WaitForExit()
            $sw.Stop()
            throw ("rodada {0} estourou {1} min — processo filho {2} encerrado" -f $r, $RoundTimeoutMin, $proc.Id)
        }
        $sw.Stop()

        # Um log único por rodada: stdout + stderr.
        $text = ""
        if (Test-Path -LiteralPath $stdout) { $text = [IO.File]::ReadAllText($stdout) }
        if (Test-Path -LiteralPath $stderr) {
            $errText = [IO.File]::ReadAllText($stderr)
            if ($errText.Trim().Length -gt 0) { $text += "`r`n--- stderr ---`r`n" + $errText }
        }
        [IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($false)))

        # PS 5.1: com Start-Process + redirecionamento, `$proc.ExitCode` pode
        # não estar acessível (o objeto devolvido não carrega o handle). Nesse
        # caso o sucesso é decidido pelos artefatos: JSON da rodada legível e
        # completo — o runner só o grava imediatamente antes do quit(0).
        $roundExit = $null
        try { $roundExit = $proc.ExitCode } catch { $roundExit = $null }
        if (($null -ne $roundExit) -and ($roundExit -ne 0)) {
            throw ("rodada {0} terminou com exit={1} (log: {2})" -f $r, $roundExit, $log)
        }
        if (-not (Test-Path -LiteralPath $roundJson)) {
            throw ("rodada {0} não gravou o JSON da rodada ({1}) — log: {2}" -f $r, $roundJson, $log)
        }
        try {
            $data = Get-Content -LiteralPath $roundJson -Raw | ConvertFrom-Json
        } catch {
            throw ("rodada {0} deixou um JSON inválido/truncado ({1}) — log: {2}" -f $r, $roundJson, $log)
        }
        if ($null -eq $data.metrics) {
            throw ("rodada {0}: JSON da rodada sem 'metrics' ({1}) — log: {2}" -f $r, $roundJson, $log)
        }

        # Rodada 1 detecta o release sem OGS_PROFILING: todas as fases em ~0.
        if ($r -eq 1) {
            $phaseSum = 0.0
            $hasPhases = $false
            if ($null -ne $data.phases) {
                foreach ($prop in $data.phases.PSObject.Properties) {
                    $phaseSum += [double]$prop.Value.mean
                    $hasPhases = $true
                }
            }
            if ((-not $hasPhases) -or ($phaseSum -eq 0)) {
                throw "release sem OGS_PROFILING: rebuild com -DOGS_PROFILE_RELEASE=ON (todas as fases vieram em 0)"
            }
        }

        $roundResults += [pscustomobject]@{
            round            = $r
            metrics          = $data.metrics
            phases           = $data.phases
            memory_static_mb = $data.meta.memory_static_mb
            log              = $log
        }

        $dayMean = if ($data.metrics.day_total_ms) { [double]$data.metrics.day_total_ms.mean } else { 0.0 }
        $tickMean = if ($data.metrics.tick_wall_ms) { [double]$data.metrics.tick_wall_ms.mean } else { 0.0 }
        Write-Host ("[a0] rodada {0}: {1:N1} s — tick parede média {2:N2} ms, dia total médio {3:N2} ms, memória {4:N1} MB" -f `
            $r, $sw.Elapsed.TotalSeconds, $tickMean, $dayMean, [double]$data.meta.memory_static_mb)
    }

    # -----------------------------------------------------------------------
    # 4. Agregação entre rodadas (mediana do mean de cada rodada).
    # -----------------------------------------------------------------------
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($res in $roundResults) {
        foreach ($block in @($res.metrics, $res.phases)) {
            if ($null -eq $block) { continue }
            foreach ($prop in $block.PSObject.Properties) {
                if (-not $names.Contains($prop.Name)) { $names.Add($prop.Name) }
            }
        }
    }

    $aggregate = [ordered]@{}
    $variations = [ordered]@{}
    $maxVar = 0.0
    foreach ($name in $names) {
        $values = @()
        foreach ($res in $roundResults) {
            $stats = $null
            if (($null -ne $res.metrics) -and ($null -ne $res.metrics.PSObject.Properties[$name])) {
                $stats = $res.metrics.$name
            } elseif (($null -ne $res.phases) -and ($null -ne $res.phases.PSObject.Properties[$name])) {
                $stats = $res.phases.$name
            }
            if ($null -ne $stats) { $values += [double]$stats.mean }
        }
        if ($values.Count -eq 0) { continue }

        $median = Get-Median $values
        $min = ($values | Measure-Object -Minimum).Minimum
        $max = ($values | Measure-Object -Maximum).Maximum
        $range = [double]$max - [double]$min
        # Variação relativa ao módulo da mediana. Mediana 0 com faixa 0 → 0%;
        # mediana 0 com faixa > 0 → 100% (divergência contra base zero, reprova).
        if ($median -eq 0) {
            $variation = if ($range -eq 0) { 0.0 } else { 100.0 }
        } else {
            $variation = $range / [math]::Abs($median) * 100.0
        }

        $aggregate[$name] = [pscustomobject]@{ median_across_rounds = $median }
        $variations[$name] = [math]::Round($variation, 4)
        # O veredito olha só as métricas-chefe (ver $criterionScope no topo).
        if (($criterionScope -contains $name) -and ($variation -gt $maxVar)) {
            $maxVar = $variation
        }
    }

    # -----------------------------------------------------------------------
    # 5. JSON final do baseline.
    # -----------------------------------------------------------------------
    $notes = "Release de medição (OGS_PROFILE_RELEASE=ON) copiado temporariamente sobre o slot debug e restaurado ao final (sha conferido); " +
        "editor fechado verificado antes de começar; threads não passadas (padrão do núcleo); " +
        "memory_static_mb vem do meta de cada rodada (OS.get_static_memory_usage com o mundo carregado, sem o mapa 3D)."

    $result = [ordered]@{
        meta    = [ordered]@{
            generated_utc      = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
            days               = $Days
            rounds             = $Rounds
            tick_mode          = "worker"
            godot              = $godotExe
            release_dll        = $releasePath
            release_sha256     = $releaseSha
            slot_sha256_before = $slotShaBefore
            host               = [ordered]@{
                cpu           = $cpu
                logical_cores = [Environment]::ProcessorCount
                os            = $osCaption
            }
            notes              = $notes
        }
        rounds        = @($roundResults)
        aggregate     = $aggregate
        variation_pct = $variations
        criterion     = [ordered]@{
            scope             = @($criterionScope)
            limit_pct         = $criterion
            max_variation_pct = [math]::Round($maxVar, 4)
            ok                = ($maxVar -le $criterion)
        }
    }

    $json = $result | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($outPath, $json, (New-Object System.Text.UTF8Encoding($false)))

    # -----------------------------------------------------------------------
    # 6. Tabela resumo (mediana das rodadas e variação %).
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host ("{0,-30} {1,16} {2,12}" -f "métrica/fase", "mediana (rodadas)", "variação %")
    Write-Host ("{0,-30} {1,16} {2,12}" -f ("-" * 30), ("-" * 16), ("-" * 12))
    foreach ($name in $names) {
        if (-not $aggregate.Contains($name)) { continue }
        Write-Host ("{0,-30} {1,16:N3} {2,12:N2}" -f $name, [double]$aggregate[$name].median_across_rounds, [double]$variations[$name])
    }
    Write-Host ""
    Write-Host ("[a0] JSON do baseline: {0}" -f $outPath)
    if ($maxVar -le $criterion) {
        Write-Host ("CRITÉRIO A0: variação máxima {0:N2}% <= {1:N0}% — OK" -f $maxVar, $criterion) -ForegroundColor Green
    } else {
        Write-Host ("CRITÉRIO A0: variação máxima {0:N2}% > {1:N0}% — FALHOU" -f $maxVar, $criterion) -ForegroundColor Red
    }
}
catch {
    Write-Host ""
    Write-Host ("ABORTADO: " + $_.Exception.Message) -ForegroundColor Red
    $exitCode = 2
}
finally {
    # Restaura SEMPRE a DLL debug original (mesmo em aborto/timeout/exceção).
    if ($swapped) {
        try {
            Copy-Item -LiteralPath $backupPath -Destination $slotPath -Force
            $slotShaAfter = Get-Sha256 $slotPath
            if ($slotShaAfter -eq $slotShaBefore) {
                Write-Host "DLL debug restaurada (sha ok)" -ForegroundColor Green
            } else {
                $restoreOk = $false
                Write-Host ("DLL debug restaurada (sha FALHOU: esperado {0}, obtido {1})" -f $slotShaBefore, $slotShaAfter) -ForegroundColor Red
            }
        } catch {
            $restoreOk = $false
            Write-Host ("DLL debug restaurada (sha FALHOU: " + $_.Exception.Message + ")") -ForegroundColor Red
        }
    }
}

if (-not $restoreOk) {
    Write-Host ""
    Write-Host "################################################################" -ForegroundColor Red
    Write-Host " ATENÇÃO: a DLL debug NÃO voltou ao slot. Restaure à mão:" -ForegroundColor Red
    Write-Host ("   copie {0}" -f $backupPath) -ForegroundColor Red
    Write-Host ("   para  {0}" -f $slotPath) -ForegroundColor Red
    Write-Host "################################################################" -ForegroundColor Red
    exit 3
}

if ($exitCode -ne 0) { exit $exitCode }
exit 0
