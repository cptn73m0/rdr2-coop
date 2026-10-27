param(
    [switch]$NoClient,
    [switch]$NoServer,
    [switch]$NoTests,
    [switch]$StrictQuality,
    [switch]$NoStrictQuality
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$python = "python"
# Strict quality gate is ON by default (P2). -NoStrictQuality disables it;
# DSH_CI_STRICT_QUALITY=0/false does too, =1/true forces it on.
$strictQuality = $true
$envStrict = $env:DSH_CI_STRICT_QUALITY
if ($envStrict -in @("0", "false", "off", "no")) { $strictQuality = $false }
elseif ($envStrict -in @("1", "true", "on", "yes")) { $strictQuality = $true }
if ($StrictQuality) { $strictQuality = $true }
if ($NoStrictQuality) { $strictQuality = $false }

function Resolve-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    # Local dev fallback: VS2019 Build Tools bundle (not required when cmake is
    # on PATH, e.g. on GitHub Actions windows-latest runners).
    $candidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "cmake not found: install CMake (PATH) or Visual Studio 2019 Build Tools"
}

function Get-ProtocolVersion {
    $protocolH = Join-Path $repo "net\protocol.h"
    if (-not (Test-Path -LiteralPath $protocolH)) { return "unknown" }
    $match = Select-String -LiteralPath $protocolH -Pattern 'kProtocolVersion\s*=\s*(\d+)\s*;' |
        Select-Object -First 1
    if ($match -and $match.Matches.Count -gt 0) {
        return $match.Matches[0].Groups[1].Value
    }
    return "unknown"
}

function Get-QualityCounts($report) {
    $highCrap = @($report.functions |
        Where-Object { [double]$_.crap_proxy -gt 5000 }).Count
    $highDups = @($report.duplication |
        Where-Object { [int]$_.count -ge 4 }).Count
    return [pscustomobject]@{
        Files = @($report.inventory).Count
        Functions = @($report.functions).Count
        HighCrap = $highCrap
        Duplicates = @($report.duplication).Count
        HighDups = $highDups
        Todos = [int]$report.markers.todos_total
    }
}

function Write-QualitySummary($report) {
    $counts = Get-QualityCounts $report
    Write-Host ("quality: files={0} functions={1} duplicates(top20)={2} dup>=4={3} crap>5000={4} TODO={5}" -f `
        $counts.Files, $counts.Functions, $counts.Duplicates, $counts.HighDups,
        $counts.HighCrap, $counts.Todos)
    Write-Host "quality top-5 CRAP:"
    foreach ($fn in @($report.functions | Select-Object -First 5)) {
        Write-Host ("  {0,8}  {1}:{2}  {3}  ({4} lines)" -f `
            $fn.crap_proxy, $fn.file, $fn.line, $fn.name, $fn.lines)
    }
}

function Get-QualityRegression($baseline, $latest) {
    $base = Get-QualityCounts $baseline
    $now = Get-QualityCounts $latest
    $regressions = @()
    if ($now.HighCrap -gt $base.HighCrap) {
        $regressions += "functions with CRAP>5000: $($base.HighCrap) -> $($now.HighCrap)"
    }
    if ($now.HighDups -gt $base.HighDups) {
        $regressions += "duplicate groups count>=4: $($base.HighDups) -> $($now.HighDups)"
    }
    if ($now.Todos -gt $base.Todos) {
        $regressions += "TODO/FIXME markers: $($base.Todos) -> $($now.Todos)"
    }
    return $regressions
}

$cmake = Resolve-CMake
$serverBuild = Join-Path $repo "build-server-ci"
$serverExe = Join-Path $serverBuild "Release\rdr2_coop_server.exe"

Write-Host "=== CI: $repo ==="
Write-Host "=== protocol: v$(Get-ProtocolVersion) (net\protocol.h) ==="
Write-Host "=== quality gate: $(if ($strictQuality) { 'strict (default)' } else { 'report only (-NoStrictQuality)' }) ==="

Write-Host "=== Quality scan (ub-mode) ==="
$qualityDir = Join-Path $repo "build"
New-Item -ItemType Directory -Force -Path $qualityDir | Out-Null
$qualityJson = Join-Path $qualityDir "quality_latest.json"
$qualityLog = Join-Path $qualityDir "quality_scan.log"
& $python (Join-Path $repo "tools\quality_scan.py") --json $qualityJson --top 20 > $qualityLog 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "quality_scan failed; see $qualityLog"
    exit $LASTEXITCODE
}
$latestQuality = Get-Content -Raw -LiteralPath $qualityJson | ConvertFrom-Json
Write-QualitySummary $latestQuality

$baselinePath = Join-Path $repo "tools\quality_baseline.json"
if (Test-Path -LiteralPath $baselinePath) {
    $baseQuality = Get-Content -Raw -LiteralPath $baselinePath | ConvertFrom-Json
    $baseCounts = Get-QualityCounts $baseQuality
    $qualityRegressions = @(Get-QualityRegression $baseQuality $latestQuality)
    Write-Host ("quality delta: crap>5000 {0} -> {1}; dup>=4 {2} -> {3}; TODO {4} -> {5}" -f `
        $baseCounts.HighCrap, (Get-QualityCounts $latestQuality).HighCrap,
        $baseCounts.HighDups, (Get-QualityCounts $latestQuality).HighDups,
        $baseCounts.Todos, (Get-QualityCounts $latestQuality).Todos)
    if ($qualityRegressions.Count -gt 0) {
        foreach ($regression in $qualityRegressions) {
            Write-Host "quality regression: $regression"
        }
        if ($strictQuality) {
            Write-Host "QUALITY GATE: FAIL (strict; baseline $baselinePath)"
            exit 1
        }
        Write-Host "QUALITY GATE: regression detected, report mode (-NoStrictQuality or DSH_CI_STRICT_QUALITY=0 disables strict)"
    } else {
        Write-Host "QUALITY GATE: PASS (no regression vs baseline)"
    }
} elseif ($strictQuality) {
    Write-Host "QUALITY GATE: FAIL (baseline missing: $baselinePath)"
    exit 1
} else {
    Write-Host "QUALITY GATE: baseline missing, report mode"
}

if (-not $NoClient) {
    Write-Host "=== Build client (ASI) ==="
    if (-not (Test-Path -LiteralPath (Join-Path $repo "build\CMakeCache.txt"))) {
        & $cmake -S $repo -B (Join-Path $repo "build") -G "Visual Studio 16 2019" -A x64
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & $cmake --build (Join-Path $repo "build") --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $NoServer) {
    Write-Host "=== Build server ==="
    if (-not (Test-Path -LiteralPath (Join-Path $serverBuild "CMakeCache.txt"))) {
        & $cmake -S (Join-Path $repo "server") -B $serverBuild -G "Visual Studio 16 2019" -A x64
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & $cmake --build $serverBuild --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $NoTests) {
    Write-Host "=== Build C++ network tests ==="
    $testBuild = Join-Path $repo "tests\build"
    if (-not (Test-Path -LiteralPath (Join-Path $testBuild "CMakeCache.txt"))) {
        & $cmake -S (Join-Path $repo "tests") -B $testBuild -G "Visual Studio 16 2019" -A x64
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & $cmake --build $testBuild --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== FrameReader fuzz ==="
    & (Join-Path $testBuild "Release\frame_reader_fuzz.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== Client reconnect ==="
    & (Join-Path $testBuild "Release\client_reconnect_test.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== Config parser ==="
    & (Join-Path $testBuild "Release\config_test.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== WorldSnapshot parser stress ==="
    & (Join-Path $testBuild "Release\world_snapshot_parse_test.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== Mission catalog/rewards ==="
    & (Join-Path $testBuild "Release\mission_catalog_test.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== world_sync candidate budget ==="
    & (Join-Path $testBuild "Release\world_entity_budget_test.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== Mission barrier rules fuzz ==="
    & (Join-Path $testBuild "Release\mission_barrier_rules_fuzz.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== v20 vehicle interaction protocol (0x23/0x24/0x26) ==="
    & (Join-Path $testBuild "Release\vehicle_interaction_protocol_test.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== Restore saves ==="
    powershell -ExecutionPolicy Bypass -File (Join-Path $repo "tests\restore_saves_test.ps1")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== Headless integration tests ==="
    # Canonical build directory is build-server-ci (same one CI uses on GitHub).
    # The protocol version is read by integration_server.py directly from
    # net/protocol.h, so a stale server build from an older protocol dir cannot
    # silently pass here.
    if (-not (Test-Path -LiteralPath $serverExe)) {
        Write-Host "server executable not found: $serverExe"
        Write-Host "Run scripts\ci.ps1 without -NoServer, or build it first:"
        Write-Host "  cmake -S server -B build-server-ci -G `"Visual Studio 16 2019`" -A x64"
        Write-Host "  cmake --build build-server-ci --config Release"
        exit 1
    }
    & $python (Join-Path $repo "tests\integration_server.py") --exe $serverExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "=== CI OK ==="
