# Gate-2 baseline capture: 30 min raw byte log from the EDV11P bridge (pure ASCII on purpose:
# PowerShell 5.1 mis-parses UTF-8-without-BOM scripts, so no CJK in this file).
param(
    [string] $Port = 'COM8',
    [int]    $Seconds = 1800,
    [string] $RepoRoot = 'C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos'
)
$ErrorActionPreference = 'Stop'
$out = Join-Path $RepoRoot 'logs\edv11p'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$raw = Join-Path $out "gate2_$stamp.bin"
$meta = Join-Path $out "gate2_$stamp.meta.txt"

function New-Frame([byte]$op, [byte]$type, [byte[]]$value, [byte]$mh, [byte]$ml) {
    $len = 11 + $value.Length
    $f = New-Object System.Collections.Generic.List[byte]
    $hdr = [byte[]](0x45, 0x44, [byte](($len -shr 8) -band 0xFF), [byte]($len -band 0xFF), $mh, $ml, $op, $type, [byte](($value.Length -shr 8) -band 0xFF), [byte]($value.Length -band 0xFF))
    $f.AddRange($hdr)
    $f.AddRange($value)
    $cs = 0
    foreach ($x in $f) { $cs = ($cs + $x) -band 0xFF }
    $f.Add([byte]$cs)
    return $f.ToArray()
}

$sp = New-Object System.IO.Ports.SerialPort($Port, 921600, 'None', 8, 'One')
$sp.ReadTimeout = 250
$sp.Open()

$fs = [System.IO.File]::Create($raw)
$buf = New-Object byte[] 4096
$total = 0
$t0 = Get-Date
$deadline = $t0.AddSeconds($Seconds)
$lastKeepalive = $t0
$mid = 0x60
"start=$($t0.ToString('o')) port=$Port seconds=$Seconds file=$raw" | Set-Content $meta -Encoding utf8

while ((Get-Date) -lt $deadline) {
    try {
        $n = $sp.Read($buf, 0, $buf.Length)
        if ($n -gt 0) { $fs.Write($buf, 0, $n); $total += $n }
    } catch [System.TimeoutException] { }
    if (((Get-Date) - $lastKeepalive).TotalSeconds -ge 60) {
        $fs.Flush()
        $mid = ($mid + 1) -band 0xFF
        $kf = New-Frame 0xA0 0x05 @() 0x60 $mid
        try { $sp.Write($kf, 0, $kf.Length) } catch { }
        $lastKeepalive = Get-Date
    }
}
$fs.Flush(); $fs.Close(); $sp.Close()
$elapsed = ((Get-Date) - $t0).TotalSeconds
"end=$((Get-Date).ToString('o')) bytes=$total elapsed_s=$([math]::Round($elapsed,1))" | Add-Content $meta -Encoding utf8
Write-Host "DONE bytes=$total elapsed_s=$([math]::Round($elapsed,1)) file=$raw"
