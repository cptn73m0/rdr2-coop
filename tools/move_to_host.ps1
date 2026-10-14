# RDR2 CO-OP — тестовый клиент «доезжает» до хостера.
# JOIN как TestBuddy, затем шлёт серию FRAME-кадров: пед-призрак движется
# по прямой из сохранённой точки до целевой и останавливается.
# Запуск: powershell -ExecutionPolicy Bypass -File tools\move_to_host.ps1

param(
    [string]$ServerHost = '127.0.0.1',
    [int]$Port = 7777,
    [string]$Name = 'TestBuddy',
    [string]$ModelHex = '0x54D8BE8E',
    [double]$TargetX = 120.0,
    [double]$TargetY = -186.0,
    [double]$TargetZ = 114.0,
    [int]$Move = 1,               # MoveState: 0=Idle, 1=Walk, 2=Run, 3=RideHorse
    [int]$State = 0,              # PedState (опционально)
    [int]$Flags = 0               # битовые флаги (опционально)
)

$ErrorActionPreference = 'Stop'

$model = [Convert]::ToUInt64($ModelHex, 16)

Write-Output "move_to_host: $Name (model $ModelHex) -> ($TargetX, $TargetY, $TargetZ)"

try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect($ServerHost, $Port)
    $s = $c.GetStream()
    Write-Output "connected to $ServerHost`:$Port"

    # --- JOIN: name_len:u8 + name + ped_model:u64 (LE) ---
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($Name)
    $pay = New-Object System.Collections.Generic.List[byte]
    $pay.Add([byte]$nameBytes.Length)
    foreach ($b in $nameBytes) { $pay.Add($b) }
    for ($i = 0; $i -lt 8; $i++) { $pay.Add([byte](($model -shr (8 * $i)) -band 0xFF)) }

    $frame = [byte[]]@([byte]$pay.Count, 0, 0, 0, 0x01) + $pay.ToArray()
    $s.Write($frame, 0, $frame.Length); $s.Flush()
    Write-Output "JOIN sent as '$Name'"

    $tmp = New-Object byte[] 64
    try { $s.ReadTimeout = 3000; $null = $s.Read($tmp, 0, 64); Write-Output "JOIN_OK received" } catch { Write-Output "no JOIN_OK (timed out)"; $s.Close(); $c.Close(); exit 1 }

    # Пауза: даём хостеру время запросить/загрузить модель и заспавнить педа,
    # иначе кадры движения придут до спавна и потеряются. LEAVE от старого
    # клиента тоже должен пройти раньше JOIN нового, поэтому ждём подольше.
    Write-Output "pausing 8s for the host to spawn the ped..."
    Start-Sleep -Seconds 8

    # --- Точка старта: та же, что в players.txt (сохранённая) ---
    $startX = -635.95; $startY = 783.97; $startZ = 112.88

    # --- Двигаемся по прямой от старта к цели, шлём FRAME ---
    $steps = 90
    function Send-Frame($x, $y, $z) {
        # FramePayload — ровно 72 байта: x,y,z,heading (16) + move,state,flags,pad (4)
        # + health (4) + weaponHash (8) + seq (8) + reserved (24) + horseOutfitHash (8).
        $pay2 = New-Object System.Collections.Generic.List[byte]
        foreach ($v in @($x,$y,$z,0.0)) {
            $b4 = [BitConverter]::GetBytes([single]$v)
            foreach ($bb in $b4) { $pay2.Add($bb) }
        }
        $pay2.Add([byte]$Move)
        $pay2.Add([byte]$State)
        $pay2.Add([byte]$Flags)
        $pay2.Add([byte]0)                              # pad0
        foreach ($bb in [BitConverter]::GetBytes([single]1.0)) { $pay2.Add($bb) }  # health
        for ($k = 0; $k -lt 8; $k++) { $pay2.Add([byte]0) }   # weaponHash
        for ($k = 0; $k -lt 8; $k++) { $pay2.Add([byte]0) }   # seq
        for ($k = 0; $k -lt 24; $k++) { $pay2.Add([byte]0) }  # reserved
        for ($k = 0; $k -lt 8; $k++) { $pay2.Add([byte]0) }   # horseOutfitHash
        $fr = [byte[]]@([byte]$pay2.Count, 0, 0, 0, 0x04) + $pay2.ToArray()
        $s.Write($fr, 0, $fr.Length); $s.Flush()
    }

    for ($k = 0; $k -le $steps; $k++) {
        $t = $k / $steps
        $x = $startX + ($TargetX - $startX) * $t
        $y = $startY + ($TargetY - $startY) * $t
        $z = $startZ + ($TargetZ - $startZ) * $t
        Send-Frame $x $y $z
        if (($k % 10) -eq 0) { Write-Output ("frame {0}: ({1:F1}, {2:F1}, {3:F1})" -f $k,$x,$y,$z) }
        Start-Sleep -Milliseconds 40
    }

    Write-Output "arrived at ($TargetX, $TargetY, $TargetZ), keeping connection (60s)..."
    Start-Sleep -Seconds 60

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