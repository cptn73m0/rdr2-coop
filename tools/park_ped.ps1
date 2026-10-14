# RDR2 CO-OP — тестовый клиент «позиционирует педа у хостера».
# JOIN как TestBuddy, пауза (модель загружается), затем КРУГОВОЙ цикл:
# каждые 2 с шлёт FRAME с целевыми координатами — пед-призрак скачет
# к цели и там «замирает». Удержание соединения 120 с.
# Запуск: powershell -ExecutionPolicy Bypass -File tools\park_ped.ps1 -TargetX 175.7 ...

param(
    [string]$ServerHost = '127.0.0.1',
    [int]$Port = 7777,
    [string]$Name = 'TestBuddy',
    [string]$ModelHex = '0x54D8BE8E',
    [double]$TargetX = 175.7,
    [double]$TargetY = -156.2,
    [double]$TargetZ = 123.9,
    [double]$TargetHeading = 0.0,
    [int]$Move = 0,               # MoveState: 0=Idle, 1=Walk, 2=Run, 3=RideHorse
    [int]$State = 0,              # PedState: 0=Normal, 1=Ragdoll, ... (опц.)
    [int]$Flags = 0,              # битовые флаги (опц.)
    [int]$HoldSeconds = 120
)

$ErrorActionPreference = 'Stop'

$model = [Convert]::ToUInt64($ModelHex, 16)

Write-Output "park_ped: $Name (model $ModelHex) -> ($TargetX, $TargetY, $TargetZ)"

try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect($ServerHost, $Port)
    $s = $c.GetStream()
    Write-Output "connected to $ServerHost`:$Port"

    # --- JOIN ---
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($Name)
    $pay = New-Object System.Collections.Generic.List[byte]
    $pay.Add([byte]$nameBytes.Length)
    foreach ($b in $nameBytes) { $pay.Add($b) }
    for ($i = 0; $i -lt 8; $i++) { $pay.Add([byte](($model -shr (8 * $i)) -band 0xFF)) }
    $frame = [byte[]]@([byte]$pay.Count, 0, 0, 0, 0x01) + $pay.ToArray()
    $s.Write($frame, 0, $frame.Length); $s.Flush()
    Write-Output "JOIN sent as '$Name'"

    $tmp = New-Object byte[] 64
    try { $s.ReadTimeout = 3000; $null = $s.Read($tmp, 0, 64); Write-Output "JOIN_OK received" } catch { Write-Output "no JOIN_OK"; $s.Close(); $c.Close(); exit 1 }

    # Пауза: даём хостеру загрузить модель и заспавнить педа.
    Write-Output "pausing 10s for model load + spawn..."
    Start-Sleep -Seconds 10

    # --- Круговой цикл: каждые 2 с телепортируем педа к цели ---
    $seq = 0
    for ($i = 0; $i -lt $HoldSeconds; $i += 2) {
        # FramePayload в протоколе — ровно 64 байта:
        # x,y,z,heading (16) + move,state,flags,pad (4) + health (4)
        # + weaponHash (8) + seq (8) + reserved (24).
        $pay2 = New-Object System.Collections.Generic.List[byte]
        foreach ($v in @([single]$TargetX,[single]$TargetY,[single]$TargetZ,[single]$TargetHeading)) {
            foreach ($bb in [BitConverter]::GetBytes($v)) { $pay2.Add($bb) }
        }
        $pay2.Add([byte]$Move)   # move — состояние движения
        $pay2.Add([byte]$State)  # state — особое состояние (0=Normal)
        $pay2.Add([byte]$Flags)  # flags — битовые (0)
        $pay2.Add([byte]0)       # pad0
        foreach ($bb in [BitConverter]::GetBytes([single]1.0)) { $pay2.Add($bb) }  # health
        for ($k = 0; $k -lt 8; $k++) { $pay2.Add([byte]0) }   # weaponHash
        for ($k = 0; $k -lt 8; $k++) { $pay2.Add([byte](($seq -shr (8 * $k)) -band 0xFF)) }  # seq
        for ($k = 0; $k -lt 24; $k++) { $pay2.Add([byte]0) }  # reserved
        $seq++
        $fr = [byte[]]@([byte]$pay2.Count, 0, 0, 0, 0x04) + $pay2.ToArray()
        $s.Write($fr, 0, $fr.Length); $s.Flush()
        if (($i % 10) -eq 0) { Write-Output ("frame sent, ped at ($TargetX, $TargetY, $TargetZ)") }
        Start-Sleep -Seconds 2
        if (-not $c.Connected) { Write-Output "connection lost"; break }
    }

    Write-Output "holding done, sending LEAVE..."
    $lf = [byte[]]@(0, 0, 0, 0, 0x05)
    try { $s.Write($lf, 0, $lf.Length); $s.Flush() } catch {}
    $s.Close(); $c.Close()
    Write-Output "done"
}
catch {
    Write-Output "ERROR: $($_.Exception.Message)"
    exit 1
}