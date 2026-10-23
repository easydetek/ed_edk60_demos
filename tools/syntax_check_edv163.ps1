# =============================================================================
# EDV163 桥接固件的编译期验证 harness（不依赖 idf.py / 命名管道）
#   * 参数取自 build-hello_board/compile_commands.json 的真实编译行
#     （-Wall -Werror -Wextra -std=gnu23 …），只替换 -I<config> 与 -fsyntax-only
#   * 6 个用例：量产配置 / 诊断打开 / 未选中本 demo(敌对配置) / dispatch 两路 /
#     断言反向验证（选中本 demo + 敌对配置，必须失败）
# 用法：pwsh -File syntax_check_edv163.ps1
# =============================================================================
$ErrorActionPreference = 'Continue'
$repo = 'C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos'
$tmp  = Join-Path $env:TEMP 'kcfg163'
$gcc  = 'C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe'

$j = Get-Content "$repo\build-hello_board\compile_commands.json" -Raw | ConvertFrom-Json
$e = $j | Where-Object { $_.file -like '*radar_bridge_edv11p*' } | Select-Object -First 1
$full = $e.command

$cflagsOrig = '@"C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/build-hello_board/toolchain/cflags"'
$cflagsInline = '-march=rv32imac_zicsr_zifencei_zaamo_zalrsc ' +
  '"-specs=C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/build-hello_board/specs/picolibc.specs"'
$cfgOrig = '-IC:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/build-hello_board/config'

$base = $full.Substring(0, $full.IndexOf(' -o '))
if ($base.IndexOf($cflagsOrig) -lt 0) { throw '没找到 cflags 响应文件参数，命令格式可能变了' }
if ($base.IndexOf($cfgOrig) -lt 0)    { throw '没找到 -I.../build-hello_board/config，命令格式可能变了' }
$base = $base.Replace($cflagsOrig, $cflagsInline)
$base = $base.Replace($gcc + ' ', '')

function Invoke-Case {
    param([string] $Name, [string] $Src, [string] $CfgDir, [bool] $ExpectOk, [string] $ExpectText)

    $line = $base.Replace($cfgOrig, "-I" + ($CfgDir -replace '\\', '/')) +
            ' -fsyntax-only "' + $Src + '"'
    $bat = Join-Path $tmp ("run_" + $Name + ".bat")
    $out = Join-Path $tmp ("out_" + $Name + ".txt")
    $txt = "@echo off`r`n`"$gcc`" $line > `"$out`" 2>&1`r`necho EXIT=%ERRORLEVEL%`r`n"
    [System.IO.File]::WriteAllText($bat, $txt, (New-Object System.Text.UTF8Encoding($false)))

    $res = (& cmd.exe /c "`"$bat`"" 2>&1 | Out-String)
    $code = if ($res -match 'EXIT=(\d+)') { [int]$Matches[1] } else { -1 }
    $body = if (Test-Path $out) { (Get-Content $out -Raw) } else { '' }
    $bodyLen = if ($body) { $body.Trim().Length } else { 0 }

    $ok = if ($ExpectOk) { ($code -eq 0) -and ($bodyLen -eq 0) }
          else           { ($code -ne 0) -and ($body -match $ExpectText) }

    $verdict = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1,-22} src={2,-28} exit={3} 输出字节={4}" -f `
        $verdict, $Name, (Split-Path $Src -Leaf), $code, $bodyLen)
    if (-not $ok) {
        Write-Host '---- 编译器输出 ----'
        Write-Host $body
    }
    elseif (-not $ExpectOk) {
        # 断言反向验证：把命中的报错行打出来作为证据
        $body -split "`n" | Where-Object { $_ -match 'error:' } | ForEach-Object { Write-Host ('    ' + $_.Trim()) }
    }
    return $ok
}

$srcNew = Join-Path $repo 'components\edk60_app\src\demo_get_started_radar_bridge_edv163.c'
$srcDisp = Join-Path $repo 'components\edk60_app\src\demo_dispatch.c'
$src11p = Join-Path $repo 'components\edk60_app\src\demo_get_started_radar_bridge_edv11p.c'

$all = @()
$all += Invoke-Case '1_main_silent'   $srcNew  (Join-Path $tmp 'cfg_main')  $true  ''
$all += Invoke-Case '2_uart0_debug'   $srcNew  (Join-Path $tmp 'cfg_dbg')   $true  ''
$all += Invoke-Case '3_not_selected'  $srcNew  (Join-Path $tmp 'cfg_other') $true  ''
$all += Invoke-Case '4_dispatch_edv163' $srcDisp (Join-Path $tmp 'cfg_main') $true ''
$all += Invoke-Case '5_dispatch_hello'  $srcDisp (Join-Path $repo 'build-hello_board\config') $true ''
$all += Invoke-Case '6_assert_must_fire' $srcNew (Join-Path $tmp 'cfg_bad')  $false 'radar_bridge_edv163'
$all += Invoke-Case '7_edv11p_untouched' $src11p (Join-Path $tmp 'cfg_main') $true  ''
$all += Invoke-Case '8_stats_via_usb'   $srcNew (Join-Path $tmp 'cfg_usbstats') $true ''
$all += Invoke-Case '9_silent_logs_off' $srcNew (Join-Path $tmp 'cfg_nosilent')  $true ''

$pass = @($all | Where-Object { $_ }).Count
Write-Output ''
Write-Output ("合计 {0} 个用例：PASS {1}，FAIL {2}" -f $all.Count, $pass, ($all.Count - $pass))
if ($pass -ne $all.Count) { exit 1 }
