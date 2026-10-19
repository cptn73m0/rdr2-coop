param(
    [string]$BackupDir = "",
    [switch]$Force
)

# Restore RDR2 co-op save backup created before mission_credit.
# Default is DRY-RUN (prints plan). Add -Force to actually copy files back.
# The backup contains manifest.txt lines: backup=<relative>|root=<absolute source root>.

$ErrorActionPreference = "Stop"

$docs = [Environment]::GetFolderPath("MyDocuments")
$base = Join-Path $docs "rdr2-coop-saves"

if (-not $BackupDir) {
    if (-not (Test-Path -LiteralPath $base)) {
        throw "Backup root not found: $base"
    }
    $BackupDir = (Get-ChildItem -LiteralPath $base -Directory |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $BackupDir -or -not (Test-Path -LiteralPath $BackupDir)) {
    throw "Backup directory not found: $BackupDir"
}

$manifestPath = Join-Path $BackupDir "manifest.txt"
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "manifest.txt not found in $BackupDir (old backup format?)"
}

function Get-NormalizedPath([string]$path) {
    return [System.IO.Path]::GetFullPath($path).TrimEnd('\', '/').ToLowerInvariant()
}

$allowed = @()
if ($env:APPDATA) {
    $allowed += Get-NormalizedPath (Join-Path $env:APPDATA ".1911\Red Dead Redemption 2\profile")
}
$allowed += Get-NormalizedPath (Join-Path $docs "Rockstar Games\Red Dead Redemption 2\Profiles")
if ($env:OneDrive) {
    $allowed += Get-NormalizedPath (Join-Path $env:OneDrive "Documents\Rockstar Games\Red Dead Redemption 2\Profiles")
}

$entries = @()
foreach ($line in Get-Content -LiteralPath $manifestPath -Encoding UTF8) {
    if ($line -match '^backup=(.+)\|root=(.+)$') {
        $entries += [pscustomobject]@{ Relative = $matches[1]; Root = $matches[2] }
    }
}
if ($entries.Count -eq 0) {
    throw "manifest.txt has no valid entries"
}

Write-Host "Backup: $BackupDir"
Write-Host "Mode:   $(if ($Force) { 'RESTORE (writes files)' } else { 'DRY-RUN (use -Force to restore)' })"
Write-Host ""

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$preRestore = Join-Path $BackupDir "restore-before-$stamp"

foreach ($entry in $entries) {
    $src = Join-Path $BackupDir $entry.Relative
    $dst = $entry.Root
    $dstNorm = Get-NormalizedPath $dst

    if (-not (Test-Path -LiteralPath $src)) {
        Write-Warning "skip (missing backup dir): $src"
        continue
    }
    $isAllowed = $false
    foreach ($a in $allowed) {
        if ($dstNorm -eq $a -or $dstNorm.StartsWith($a + "\")) { $isAllowed = $true; break }
    }
    if (-not $isAllowed) {
        Write-Warning "skip (target outside known RDR2 save roots): $dst"
        continue
    }

    Write-Host "RESTORE $src"
    Write-Host "     -> $dst"
    if (-not $Force) { continue }

    # Сначала сохраняем текущее состояние целевого каталога.
    if (Test-Path -LiteralPath $dst) {
        $pre = Join-Path $preRestore $entry.Relative
        New-Item -ItemType Directory -Path $pre -Force | Out-Null
        Copy-Item -Path (Join-Path $dst "*") -Destination $pre -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "     current state -> $pre"
    } else {
        New-Item -ItemType Directory -Path $dst -Force | Out-Null
    }

    Get-ChildItem -LiteralPath $src -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $dst -Recurse -Force
    }
    Write-Host "     restored"
}

if (-not $Force) {
    Write-Host ""
    Write-Host "Dry-run complete. Re-run with -Force to restore."
} else {
    Write-Host ""
    Write-Host "Restore complete. Pre-restore snapshot: $preRestore"
}
