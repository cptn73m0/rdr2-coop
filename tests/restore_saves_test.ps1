# Offline test for scripts\restore_saves.ps1 using a temp backup + target root.
$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("rdr2coop-restore-" + [guid]::NewGuid().ToString("N"))
$backup = Join-Path $tmp "backup"
$target = Join-Path $tmp "target"
New-Item -ItemType Directory -Path (Join-Path $backup "profile") -Force | Out-Null
New-Item -ItemType Directory -Path $target -Force | Out-Null

# Backup contains "restored"; current target contains "current".
Set-Content -LiteralPath (Join-Path $backup "profile\SRDRTEST") -Value "restored" -Encoding ASCII
Set-Content -LiteralPath (Join-Path $target "SRDRTEST") -Value "current" -Encoding ASCII
Set-Content -LiteralPath (Join-Path $backup "manifest.txt") `
    -Value ("backup=profile|root=" + $target) -Encoding UTF8

try {
    & (Join-Path $repo "scripts\restore_saves.ps1") -BackupDir $backup -Force -AllowedRoots @($target)
    # restore_saves.ps1 is a PowerShell script, so it does not set
    # $LASTEXITCODE; verify effects below instead.

    $restored = Get-Content -LiteralPath (Join-Path $target "SRDRTEST") -Raw
    if ($restored.Trim() -ne "restored") {
        throw "target file was not restored (got '$restored')"
    }

    $pre = Get-ChildItem -LiteralPath $backup -Directory -Filter "restore-before-*" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $pre) { throw "pre-restore snapshot not created" }
    $before = Get-Content -LiteralPath (Join-Path $pre.FullName "profile\SRDRTEST") -Raw
    if ($before.Trim() -ne "current") {
        throw "pre-restore snapshot does not contain current state (got '$before')"
    }

    Write-Host "restore_saves_test: PASS"
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
