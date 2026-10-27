<#
.SYNOPSIS
  Деплой сборки из dist\<Label> на второй ПК (ARINA): ASI + сервер + INI-ключи, с проверкой хешей.

.DESCRIPTION
  Берёт самодостаточный снапшот из dist\<Label>\ (rdr2_coop.asi, rdr2_coop_server.exe,
  rdr2-coop.ini.local), копирует ASI и сервер на ARINA, мержит фича-ключи в INI ARINA
  (identity-ключи: server_host/server_port/player_name/ped_model и др. остаются как есть)
  и сверяет SHA256. Ничего не запускает.

  Ожидает, что share \\ARINA доступен (второй ПК включён). Если share offline — выходит
  с кодом 2 и ничего не меняет.

.PARAMETER Label
  Имя снапшота в dist (например npc-e13).

.PARAMETER CheckOnly
  Только проверить доступность share/хеши, ничего не копировать.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\deploy_arina.ps1 -Label npc-e13
#>
param(
    [Parameter(Mandatory = $true)][string]$Label,
    [switch]$CheckOnly,
    [string]$RepoRoot = "",
    [string]$ArinaScripts = "\\ARINA\Red Dead Redemption 2\scripts",
    [string]$ArinaServerDir = "\\ARINA\Red Dead Redemption 2\coop-server"
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $RepoRoot = Split-Path -Parent $scriptDir
}

$snapDir = Join-Path (Join-Path $RepoRoot 'dist') $Label
if (-not (Test-Path -LiteralPath $snapDir)) {
    Write-Host "ERROR: snapshot not found: $snapDir"
    exit 2
}
$asiSrc = Join-Path $snapDir 'rdr2_coop.asi'
$srvSrc = Join-Path $snapDir 'rdr2_coop_server.exe'
$iniSrc = Join-Path $snapDir 'rdr2-coop.ini.local'
foreach ($p in @($asiSrc, $srvSrc)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host "ERROR: missing $p"; exit 2 }
}

$asiHash = (Get-FileHash -LiteralPath $asiSrc -Algorithm SHA256).Hash
$srvHash = (Get-FileHash -LiteralPath $srvSrc -Algorithm SHA256).Hash

Write-Host "Snapshot: $Label"
Write-Host "  ASI:    $asiHash"
Write-Host "  Server: $srvHash"

if (-not (Test-Path -LiteralPath $ArinaScripts)) {
    Write-Host "ARINA OFFLINE: share not reachable ($ArinaScripts). Nothing changed."
    exit 2
}

if ($CheckOnly) {
    Write-Host "CheckOnly: ARINA reachable, snapshot hashes OK."
    exit 0
}

$dstAsi = Join-Path $ArinaScripts 'rdr2_coop.asi'
$dstIni = Join-Path $ArinaScripts 'rdr2-coop.ini'
New-Item -ItemType Directory -Path $ArinaServerDir -Force | Out-Null
$dstSrv = Join-Path $ArinaServerDir 'rdr2_coop_server.exe'

# ASI / server: copy only if unlocked (try to open for write).
function Test-Unlocked([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $true }
    try {
        $fs = [System.IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
        $fs.Close()
        return $true
    } catch { return $false }
}

if (-not (Test-Unlocked $dstAsi)) {
    Write-Host "ARINA .asi is LOCKED (game running?). Close the game on ARINA and retry. Nothing changed."
    exit 3
}
if (-not (Test-Unlocked $dstSrv)) {
    Write-Host "ARINA server exe is LOCKED (server running?). Stop it and retry. Nothing changed."
    exit 3
}

Copy-Item -LiteralPath $asiSrc -Destination $dstAsi -Force
$newAsiHash = (Get-FileHash -LiteralPath $dstAsi -Algorithm SHA256).Hash
if ($newAsiHash -ne $asiHash) { Write-Host "ERROR: ASI hash mismatch after copy: $newAsiHash"; exit 4 }
Write-Host "ASI copied -> $dstAsi ($($newAsiHash.Substring(0,16))...)"

Copy-Item -LiteralPath $srvSrc -Destination $dstSrv -Force
$newSrvHash = (Get-FileHash -LiteralPath $dstSrv -Algorithm SHA256).Hash
if ($newSrvHash -ne $srvHash) { Write-Host "ERROR: server hash mismatch after copy: $newSrvHash"; exit 4 }
Write-Host "Server copied -> $dstSrv ($($newSrvHash.Substring(0,16))...)"

# INI merge: feature keys only; identity/personal keys stay untouched.
$identityPrefixes = @(
    'server_host', 'server_port', 'player_name', 'ped_model', 'save_backup',
    'mission_reward_log', 'mission_credit'
)
$featurePrefixes = @(
    'world_sync', 'action_sync', 'vehicle_sync', 'mission_sync',
    'world_vehicles', 'world_trains', 'train_debug',
    'vehicle_interaction',
    'npc_', 'player_frame_'
)

function Read-IniMap([string]$path) {
    $map = [ordered]@{}
    if (-not (Test-Path -LiteralPath $path)) { return $map }
    foreach ($line in (Get-Content -LiteralPath $path -Encoding UTF8)) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith(';') -or $t.StartsWith('#')) { continue }
        if ($t -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') {
            $map[$Matches[1]] = $Matches[2]
        }
    }
    return $map
}

if (Test-Path -LiteralPath $iniSrc) {
    $srcMap = Read-IniMap $iniSrc
    $dstMap = Read-IniMap $dstIni
    $changed = 0
    foreach ($k in $srcMap.Keys) {
        $isFeature = $false
        foreach ($p in $featurePrefixes) { if ($k -eq $p -or $k.StartsWith($p)) { $isFeature = $true; break } }
        if (-not $isFeature) { continue }
        if (-not $dstMap.Contains($k) -or $dstMap[$k] -ne $srcMap[$k]) { $changed++ }
        $dstMap[$k] = $srcMap[$k]
    }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('; RDR2 co-op INI — обновлено tools\deploy_arina.ps1 из dist\' + $Label)
    foreach ($k in $dstMap.Keys) { $lines.Add("$k = $($dstMap[$k])") }
    $text = ($lines -join "`n") + "`n"
    [System.IO.File]::WriteAllText($dstIni, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "INI merged: $dstIni (changed/added feature keys: $changed)"
} else {
    Write-Host "WARNING: snapshot has no rdr2-coop.ini.local; INI on ARINA not changed."
}

Write-Host ""
Write-Host "DONE. On ARINA:"
Write-Host "  1) launch RDR2 (guest) with the new ASI"
Write-Host "  2) server on the host PC must be running (v19 $srvHash.Substring(0,8))"
Write-Host "  3) do NOT start a second server on ARINA unless it is the host."
exit 0
