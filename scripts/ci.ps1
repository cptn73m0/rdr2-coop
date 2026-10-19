param(
    [switch]$NoClient,
    [switch]$NoServer,
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$python = "python"

Write-Host "=== CI: $repo ==="

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
    $serverBuild = Join-Path $repo "build-server-ci"
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
    $exe = Join-Path $repo "build-server-ci\Release\rdr2_coop_server.exe"
    if (-not (Test-Path -LiteralPath $exe)) {
        $exe = Join-Path $repo "build-server-v12\Release\rdr2_coop_server.exe"
    }
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Host "server executable not found"
        exit 1
    }
    & $python (Join-Path $repo "tests\integration_server.py") --exe $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "=== CI OK ==="
