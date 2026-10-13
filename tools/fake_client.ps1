# RDR2 CO-OP — тестовый клиент (эмуляция второго игрока).
# Подключается к серверу-релею, выполняет JOIN, держит соединение открытым,
# затем (по таймауту) шлёт LEAVE. Используется для проверки спавна педа
# удалённого игрока, его смерти, метки и оповещений в игре.
#
# Запуск (пример):
#   powershell -ExecutionPolicy Bypass -File tools\fake_client.ps1 -Name TestBuddy -Seconds 300

param(
    [string]$ServerHost = '127.0.0.1',
    [int]$Port = 7777,
    [string]$Name = 'TestBuddy',
    [string]$ModelHex = '0x54D8BE8E',   # a_m_m_rancher_01
    [int]$Seconds = 300
)

$ErrorActionPreference = 'Stop'

$model = [Convert]::ToUInt64($ModelHex, 16)

Write-Output "RDR2 CO-OP fake client: $Name (model $ModelHex) -> $ServerHost`:$Port"

try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect($ServerHost, $Port)
    $s = $c.GetStream()
    Write-Output "connected to $Host`:$Port"

    # --- JOIN: name_len:u8 + name + ped_model:u64 (LE) ---
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($Name)
    $pay = New-Object System.Collections.Generic.List[byte]
    $pay.Add([byte]$nameBytes.Length)
    foreach ($b in $nameBytes) { $pay.Add($b) }
    for ($i = 0; $i -lt 8; $i++) { $pay.Add([byte](($model -shr (8 * $i)) -band 0xFF)) }

    $frame = [byte[]]@([byte]$pay.Count, 0, 0, 0, 0x01) + $pay.ToArray()
    $s.Write($frame, 0, $frame.Length); $s.Flush()
    Write-Output "JOIN sent as '$Name'"

    # --- прислоняется JOIN_OK ---
    $tmp = New-Object byte[] 64
    try { $s.ReadTimeout = 3000; $null = $s.Read($tmp, 0, 64); Write-Output "JOIN_OK received" } catch { Write-Output "no JOIN_OK (timed out)" }

    Write-Output "keeping connection open for $Seconds s (пед удалённого игрока должен заспавниться в игре)..."
    for ($i = 0; $i -lt $Seconds; $i++) {
        Start-Sleep -Seconds 1
        if (-not $c.Connected) { Write-Output "connection lost early"; break }
    }

    # --- LEAVE ---
    $lf = [byte[]]@(0, 0, 0, 0, 0x05)
    try { $s.Write($lf, 0, $lf.Length); $s.Flush(); Write-Output "LEAVE sent" } catch {}
    $s.Close(); $c.Close()
    Write-Output "done"
}
catch {
    Write-Output "ERROR: $($_.Exception.Message)"
    exit 1
}