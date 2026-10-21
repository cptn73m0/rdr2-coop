param(
    [string]$BuildDir = "build-server-ci",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

function Resolve-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    $candidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "cmake not found: install CMake (PATH) or Visual Studio 2019 Build Tools"
}

function Get-ProtocolVersion {
    $protocolH = Join-Path $repo "net\protocol.h"
    $match = Select-String -LiteralPath $protocolH -Pattern 'kProtocolVersion\s*=\s*(\d+)\s*;' |
        Select-Object -First 1
    if ($match -and $match.Matches.Count -gt 0) {
        return $match.Matches[0].Groups[1].Value
    }
    return "unknown"
}

if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $buildPath = $BuildDir
} else {
    $buildPath = Join-Path $repo $BuildDir
}
$exe = Join-Path $buildPath "Release\rdr2_coop_server.exe"

if (-not $NoBuild) {
    $cmake = Resolve-CMake
    if (-not (Test-Path -LiteralPath (Join-Path $buildPath "CMakeCache.txt"))) {
        Write-Host "Configuring server ($BuildDir)..."
        & $cmake -S (Join-Path $repo "server") -B $buildPath -G "Visual Studio 16 2019" -A x64
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    Write-Host "Building server ($BuildDir)..."
    & $cmake --build $buildPath --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "Server executable not found: $exe"
    Write-Host "Build it first or drop -NoBuild."
    exit 1
}

$protocolH = Join-Path $repo "net\protocol.h"
if ((Get-Item -LiteralPath $exe).LastWriteTime -lt (Get-Item -LiteralPath $protocolH).LastWriteTime) {
    Write-Host "WARNING: server exe is older than net\protocol.h; tests may hit VersionMismatch."
    Write-Host "         Rebuild without -NoBuild if JOIN tests fail."
}

Write-Host "=== integration_server.py: protocol v$(Get-ProtocolVersion) from net\protocol.h ==="
python (Join-Path $repo "tests\integration_server.py") --exe $exe
exit $LASTEXITCODE
