param(
    [string]$BuildDir = "build-server-v12",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$exe = Join-Path $repo "$BuildDir\Release\rdr2_coop_server.exe"

if (-not $NoBuild) {
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Host "Building server ($BuildDir)..."
        & $cmake -S (Join-Path $repo "server") -B (Join-Path $repo $BuildDir) -G "Visual Studio 16 2019" -A x64
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & $cmake --build (Join-Path $repo $BuildDir) --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

python (Join-Path $repo "tests\integration_server.py") --exe $exe
exit $LASTEXITCODE
