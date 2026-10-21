param(
    [switch]$NoClient,
    [switch]$NoServer,
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$python = "python"

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

$cmake = Resolve-CMake
$serverBuild = Join-Path $repo "build-server-ci"
$serverExe = Join-Path $serverBuild "Release\rdr2_coop_server.exe"

Write-Host "=== CI: $repo ==="
Write-Host "=== protocol: v$(Get-ProtocolVersion) (net\protocol.h) ==="

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
