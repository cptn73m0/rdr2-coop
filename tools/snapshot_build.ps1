#requires -Version 5.1
<#
.SYNOPSIS
  Сохраняет сборку (ASI + сервер + INI) в dist\<Label>\ с README-описанием и хешами.

.DESCRIPTION
  Каждый снапшот — самодостаточный откат: клиентский .asi, серверный .exe, копия INI
  и README.md с версией протокола, коммитом/тегом, фичами, хешами и порядком восстановления.

.PARAMETER Label
  Имя папки снапшота (например npc-e8.1). Создаётся dist\<Label>\.

.PARAMETER Features
  Описание фич сборки (попадает в README).

.PARAMETER Protocol
  Версия протокола (например 19). Если не указана — читается из net\protocol.h.

.PARAMETER Commit
  Коммит сборки. Если не указан — текущий HEAD.

.PARAMETER Tag
  Тег сборки (необязательно).

.PARAMETER Note
  Дополнительные заметки/ограничения (необязательно).

.EXAMPLE
  pwsh -NoProfile -ExecutionPolicy Bypass -File tools\snapshot_build.ps1 -Label npc-e8.1 -Features "..." -Tag npc-e8.1
#>
param(
    [Parameter(Mandatory = $true)][string]$Label,
    [Parameter(Mandatory = $true)][string]$Features,
    [string]$Protocol = "",
    [string]$Commit = "",
    [string]$Tag = "",
    [string]$Note = "",
    [string]$RepoRoot = "",
    [string]$AsiPath = "",
    [string]$ServerExePath = "",
    [string]$IniLocalPath = "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.ini",
    [string]$IniArinaPath = "\\ARINA\Red Dead Redemption 2\scripts\rdr2-coop.ini"
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $RepoRoot = Split-Path -Parent $scriptDir
}
if (-not $RepoRoot) { $RepoRoot = (Get-Location).Path }

if (-not $AsiPath) { $AsiPath = Join-Path $RepoRoot 'build\Release\rdr2_coop.asi' }
if (-not $ServerExePath) { $ServerExePath = Join-Path $RepoRoot 'build-server\Release\rdr2_coop_server.exe' }

$dest = Join-Path (Join-Path $RepoRoot 'dist') $Label
New-Item -ItemType Directory -Path $dest -Force | Out-Null

function Copy-IfExists([string]$src, [string]$dstName) {
    if (Test-Path -LiteralPath $src) {
        $dst = Join-Path $dest $dstName
        $srcFull = (Resolve-Path -LiteralPath $src).Path
        $dstFull = [System.IO.Path]::GetFullPath($dst)
        if ($srcFull -ne $dstFull) {
            Copy-Item -LiteralPath $src -Destination $dst -Force
        }
        $hash = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash
        $len = (Get-Item -LiteralPath $dst).Length
        return [pscustomobject]@{ Name = $dstName; Size = $len; SHA256 = $hash; Source = $src }
    }
    return [pscustomobject]@{ Name = $dstName; Size = 0; SHA256 = 'MISSING'; Source = $src }
}

$items = @()
$items += Copy-IfExists $AsiPath 'rdr2_coop.asi'
$items += Copy-IfExists $ServerExePath 'rdr2_coop_server.exe'

$iniLines = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $IniLocalPath) {
    Copy-Item -LiteralPath $IniLocalPath -Destination (Join-Path $dest 'rdr2-coop.ini.local') -Force
    $iniLines.Add('Локальный ПК (`E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.ini`):')
    $iniLines.Add('```ini')
    foreach ($l in (Get-Content -LiteralPath $IniLocalPath -Encoding UTF8)) { $iniLines.Add($l) }
    $iniLines.Add('```')
} else {
    $iniLines.Add('Локальный INI: НЕ НАЙДЕН на момент снапшота.')
}
$arinaOnline = Test-Path -LiteralPath '\\ARINA\Red Dead Redemption 2\scripts'
if ($arinaOnline -and (Test-Path -LiteralPath $IniArinaPath)) {
    Copy-Item -LiteralPath $IniArinaPath -Destination (Join-Path $dest 'rdr2-coop.ini.arina') -Force
    $iniLines.Add('')
    $iniLines.Add('ARINA (`\\ARINA\Red Dead Redemption 2\scripts\rdr2-coop.ini`):')
    $iniLines.Add('```ini')
    foreach ($l in (Get-Content -LiteralPath $IniArinaPath -Encoding UTF8)) { $iniLines.Add($l) }
    $iniLines.Add('```')
} else {
    $iniLines.Add('')
    $iniLines.Add('ARINA INI: не сохранён (ARINA была offline на момент снапшота).')
}

if (-not $Protocol) {
    $protoFile = Join-Path $RepoRoot 'net\protocol.h'
    if (Test-Path -LiteralPath $protoFile) {
        $m = Select-String -LiteralPath $protoFile -Pattern 'kProtocolVersion\s*=\s*(\d+)' | Select-Object -First 1
        if ($m -and $m.Matches.Count -gt 0) { $Protocol = $m.Matches[0].Groups[1].Value }
    }
    if (-not $Protocol) { $Protocol = 'unknown' }
}
if (-not $Commit) {
    Push-Location $RepoRoot
    try { $Commit = (git rev-parse --short HEAD 2>$null) } catch { $Commit = 'unknown' }
    Pop-Location
    if (-not $Commit) { $Commit = 'unknown' }
}

$asiItem = $items | Where-Object { $_.Name -eq 'rdr2_coop.asi' }
$srvItem = $items | Where-Object { $_.Name -eq 'rdr2_coop_server.exe' }
$dateStr = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz')

$readme = New-Object System.Collections.Generic.List[string]
$readme.Add("# Снапшот сборки: $Label")
$readme.Add('')
$readme.Add("- Дата снапшота: $dateStr")
$readme.Add("- Протокол: v$Protocol")
$readme.Add("- Коммит: $Commit")
if ($Tag) { $readme.Add("- Тег: $Tag") }
$readme.Add('')
$readme.Add('## Фичи')
$readme.Add($Features)
$readme.Add('')
if ($Note) { $readme.Add('## Заметки / ограничения'); $readme.Add($Note); $readme.Add('') }
$readme.Add('## Артефакты и хеши (SHA256)')
$readme.Add('')
$readme.Add('| Файл | Размер | SHA256 |')
$readme.Add('|---|---|---|')
$readme.Add("| rdr2_coop.asi | $($asiItem.Size) | ``$($asiItem.SHA256)`` |")
$readme.Add("| rdr2_coop_server.exe | $($srvItem.Size) | ``$($srvItem.SHA256)`` |")
$readme.Add('')
$readme.Add('## INI')
$readme.Add('')
foreach ($l in $iniLines) { $readme.Add($l) }
$readme.Add('')
$readme.Add('## Как восстановить (откат на эту сборку)')
$readme.Add('')
$readme.Add('1. Закрыть игры (RDR2 на обоих ПК) и остановить сервер.')
$readme.Add('2. Клиент: скопировать `rdr2_coop.asi` из этой папки в `E:\Games\Red Dead Redemption 2\scripts\rdr2_coop.asi`.')
$readme.Add('3. Сервер: скопировать `rdr2_coop_server.exe` в `E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe`.')
$readme.Add('4. INI: привести `rdr2-coop.ini` к значениям из раздела INI (неизвестные старым сборкам ключи игнорируются).')
$readme.Add('5. Запустить сервер в ВИДИМОМ окне: `build-server\Release\rdr2_coop_server.exe 7777`.')
$readme.Add("6. Проверить версию протокола на проводе (JOIN v$Protocol → JoinOk).")
$readme.Add('')
$readme.Add('## Как задеплоить на ARINA (когда она в сети)')
$readme.Add('')
$readme.Add('1. Скопировать `rdr2_coop.asi` → `\\ARINA\Red Dead Redemption 2\scripts\rdr2_coop.asi`.')
$readme.Add('2. Скопировать `rdr2_coop_server.exe` → `\\ARINA\Red Dead Redemption 2\coop-server\rdr2_coop_server.exe` (если требуется сервер на ARINA).')
$readme.Add('3. Привести INI ARINA к значениям из раздела INI.')
$readme.Add('')
$readme.Add('Все снапшоты лежат в `E:\rdr2-coop\dist\<версия>\`; этот файл — самодостаточное описание отката.')
$readme.Add('')

Set-Content -LiteralPath (Join-Path $dest 'README.md') -Value ($readme -join "`n") -Encoding UTF8

Write-Host "Snapshot: dist\$Label"
Write-Host "  ASI:    $($asiItem.SHA256) ($($asiItem.Size))"
Write-Host "  Server: $($srvItem.SHA256) ($($srvItem.Size))"
Write-Host "  Protocol: v$Protocol, commit $Commit"
