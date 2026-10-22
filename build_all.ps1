#Requires -Version 5.1
<#
=============================================================================
  EDK60 多工程 Demo 构建脚本

  用法（在仓库根目录执行）：
   .\build_all.ps1                       # 编译全部 demo（每个 demo 一个独立 build 目录）
   .\build_all.ps1 -Demo blink           # 只编译指定 demo
   .\build_all.ps1 -Demo blink,button,wifi_sta
   .\build_all.ps1 -List                 # 只列出 demo 清单
   .\build_all.ps1 -Clean                # 编译前删除对应 build-<demo> 目录（会打印删除清单）
   .\build_all.ps1 -Size                 # 额外输出每个固件的体积明细

  设计要点：
   * demo 切换方式：把 `CONFIG_<symbol>=y` 写进 `sdkconfig.defaults.esp32c6`，
     再由 idf.py 用 `-D SDKCONFIG=sdkconfig.<demo>` 生成/复用每个 demo 独立的配置。
     好处是：不需要 menuconfig 交互，且与 VS Code 插件共用同一套 Kconfig 机制。
   * 每个 demo 一个 build-<demo>/ 目录，互不污染，可反复增量编译。
   * 本脚本只「写 sdkconfig.defaults.esp32c6 / 新建 build 目录」，
     删除动作（-Clean）会先把清单打印出来，并受 -Confirm 控制。
   * IDF 环境由脚本内的 activation script 注入，无需预先打开 IDF 终端。
=============================================================================
#>
[CmdletBinding()]
param(
    [string[]] $Demo,
    [switch] $List,
    [switch] $Clean,
    [switch] $Size
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = $PSScriptRoot
$Activate = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$DefaultsTarget = Join-Path $RepoRoot 'sdkconfig.defaults.esp32c6'

# demo 名称 -> Kconfig 符号（与 main/Kconfig.projbuild 一一对应）
$DemoMap = [ordered]@{
    'hello_board'       = 'APP_EDK60_DEMO_HELLO_BOARD'
    'blink'             = 'APP_EDK60_DEMO_BLINK'
    'button'            = 'APP_EDK60_DEMO_BUTTON'
    'uart_echo'         = 'APP_EDK60_DEMO_UART_ECHO'
    'adc_read'          = 'APP_EDK60_DEMO_ADC_READ'
    'wifi_sta'          = 'APP_EDK60_DEMO_WIFI_STA'
    'wifi_softap'       = 'APP_EDK60_DEMO_WIFI_SOFTAP'
    'ble_uart'          = 'APP_EDK60_DEMO_BLE_UART'
    'ble_scan'          = 'APP_EDK60_DEMO_BLE_SCAN'
    'radar_power'       = 'APP_EDK60_DEMO_RADAR_POWER'
    'led_sequence'      = 'APP_EDK60_DEMO_LED_SEQUENCE'
    'i2c_scan'          = 'APP_EDK60_DEMO_I2C_SCAN'
    'deep_sleep'        = 'APP_EDK60_DEMO_DEEP_SLEEP'
    'nvs_rw'            = 'APP_EDK60_DEMO_NVS_RW'
    'usb_serial_bridge' = 'APP_EDK60_DEMO_USB_SERIAL_BRIDGE'
    'radar_bridge'      = 'APP_EDK60_DEMO_RADAR_BRIDGE'
    'radar_bridge_edv11p' = 'APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P'
}

if ($List) {
    Write-Host "可用 demo（共 $($DemoMap.Count) 个）：" -ForegroundColor Cyan
    $i = 0
    foreach ($k in @($DemoMap.Keys)) {
        $i++
        Write-Host ("  {0,2}. {1,-19} -> CONFIG_{2}" -f $i, $k, $DemoMap[$k])
    }
    return
}

$Targets = @(if ($Demo) { $Demo } else { @($DemoMap.Keys) })
foreach ($t in $Targets) {
    if (-not $DemoMap.Contains($t)) {
        throw "未知 demo: '$t'。可用：$((@($DemoMap.Keys)) -join ', ')"
    }
}

# ---------------------------------------------------------------------------
# 删除清单预检（删除操作守则：先列清单，再执行）
# ---------------------------------------------------------------------------
$dirsToClean = @()
if ($Clean) {
    foreach ($t in $Targets) {
        $d = Join-Path $RepoRoot "build-$t"
        if (Test-Path $d) { $dirsToClean += $d }
    }
    if ($dirsToClean.Count -gt 0) {
        Write-Host '即将删除以下构建目录（仅构建产物，不含源码）：' -ForegroundColor Yellow
        foreach ($d in $dirsToClean) { Write-Host "  - $d" -ForegroundColor Yellow }
    }
    else {
        Write-Host '没有需要清理的构建目录。' -ForegroundColor DarkGray
    }
}

# ---------------------------------------------------------------------------
# 注入 ESP-IDF 环境
# ---------------------------------------------------------------------------
if (-not (Test-Path $Activate)) { throw "找不到 ESP-IDF 激活脚本：$Activate" }
Write-Host "注入 ESP-IDF 环境 ..." -ForegroundColor DarkGray
# 激活脚本内部使用 $ErrorActionPreference='Stop' + StrictMode 敏感写法（查找 eim 命令），
# 在脚本作用域下会误报。这里临时放宽严格模式，注入完成后再恢复。
$__oldEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off
. $Activate | Out-Null
Set-StrictMode -Version Latest
$ErrorActionPreference = $__oldEap
if (-not $env:IDF_PATH) { throw 'IDF_PATH 未设置，环境注入失败' }
Write-Host ("  ESP-IDF {0}   IDF_PATH={1}" -f $env:IDF_VERSION, $env:IDF_PATH) -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------
function Write-DemoConfig {
    param([string] $Symbol)
    # 覆盖式写入：只放「当前 demo」这一条选择，其余都靠 sdkconfig.defaults
    #
    # 2026-10-22 修复：原先用 `Set-Content -Encoding UTF8`（PS 5.1 会加 BOM），
    # IDF 的 kconfgen 把 BOM 当正文，实测报
    #   WARNING: ... ignoring malformed line '锘? 鐢?build_all.ps1 鑷姩鐢熸垚 ...'
    # 虽然只是首行注释被忽略、CONFIG 仍生效，但属于噪音且脆弱。
    # 改为「无 BOM 的 UTF-8」，从根源消除该警告。
    $content = @(
        '# 由 build_all.ps1 自动生成 —— 请勿手工编辑，改动会被下一次构建覆盖。',
        "# 当前 demo 选择：$Symbol",
        "CONFIG_$Symbol=y",
        ''
    ) -join "`r`n"
    [System.IO.File]::WriteAllText($DefaultsTarget, $content, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-FirmwareSize {
    param([string] $BuildDir)
    $bin = Join-Path $BuildDir 'edk60_demos.bin'
    if (Test-Path $bin) {
        return [math]::Round((Get-Item $bin).Length / 1KB, 1)
    }
    return 0
}

function Invoke-OneDemoBuild {
    param([string] $Name)

    $symbol   = $DemoMap[$Name]
    $buildDir = Join-Path $RepoRoot "build-$Name"
    $sdkcfg   = Join-Path $RepoRoot "sdkconfig.$Name"
    $sw       = [System.Diagnostics.Stopwatch]::StartNew()
    $log      = Join-Path $env:TEMP "edk60_build_$Name.log"

    Write-Host ''
    Write-Host ('=' * 78) -ForegroundColor Cyan
    Write-Host ("[EDK60] 构建 demo: {0}    (CONFIG_{1})" -f $Name, $symbol) -ForegroundColor Cyan
    Write-Host ('=' * 78) -ForegroundColor Cyan

    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host ("  [clean] 删除构建目录: {0}" -f $buildDir) -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }

    Write-DemoConfig -Symbol $symbol

    # 每次构建都重新生成该 demo 的 sdkconfig。
    # 原因：Kconfig 新增/改名符号后，若复用旧 sdkconfig，新符号不会被生成，
    # 就会出现「代码里引用的 CONFIG_xxx 未定义」这种看起来莫名其妙的编译错误。
    # 该文件是完全由 sdkconfig.defaults 派生的产物，删掉后重建代价很小。
    if (Test-Path $sdkcfg) {
        Remove-Item $sdkcfg -Force
    }

    Push-Location $RepoRoot
    try {
        # -D IDF_TARGET 与 -D SDKCONFIG 一起传，保证「目标芯片 + demo 选择」都是确定的，
        # 不依赖 build 目录里上一次的状态。
        #
        # SDKCONFIG_DEFAULTS：工程级 defaults + （可选的）demo 专用 defaults。
        # ESP-IDF 支持分号分隔的多个文件，**后面的覆盖前面的**，
        # 因此 demo 专用文件可以只写需要覆盖的那几行。
        # 目前用到它的场景：radar_bridge 需要关掉「控制台第二输出口=USB」，
        # 否则日志会与控制台日志混进同一个 USB 串口，污染上位机数据流。
        $extraDefaults = Join-Path $RepoRoot "sdkconfig.defaults.$Name"
        $defaultsArg = if (Test-Path $extraDefaults) {
            "$RepoRoot\sdkconfig.defaults;$extraDefaults"
        } else {
            "$RepoRoot\sdkconfig.defaults"
        }

        # 2026-10-22 修复（R13）：IDF 6.1 的 Kconfig 会向 stderr 打 NOTE/WARNING
        # （例如 BT_NIMBLE_MESH_PROVISIONER: 'default 0' is not a valid bool value）。
        # 顶层 $ErrorActionPreference='Stop' 会把 `2>&1` 收进来的 stderr 记录当成
        # 终止性错误抛出 -> 「构建其实在跑，脚本却报 [异常] 并判 FAIL」的假失败。
        # 这里临时放宽为 Continue，成败只认 $LASTEXITCODE。
        $__buildEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $out = & idf.py -B $buildDir -D "IDF_TARGET=esp32c6" -D "SDKCONFIG=$sdkcfg" `
                        -D "SDKCONFIG_DEFAULTS=$defaultsArg" build 2>&1
        $code = $LASTEXITCODE
        $ErrorActionPreference = $__buildEap
        $out | Set-Content -Path $log -Encoding UTF8
        # 只回显关键行，避免刷屏
        $out | Select-String -Pattern '\[EDK60\]|error|Error|ERROR|warning:|失败|FAILED|Successfully created|Project build complete|\.bin' |
            Select-Object -First 40 |
            ForEach-Object { Write-Host ("  " + $_.Line) }
    }
    catch {
        $code = 1
        Write-Host ("  [异常] " + $_.Exception.Message) -ForegroundColor Red
    }
    finally {
        Pop-Location
        $sw.Stop()
    }

    $ok = ($code -eq 0)
    $status = if ($ok) { 'OK' } else { "FAIL($code)" }
    if (-not $ok) {
        Write-Host ("  编译失败，完整日志：{0}" -f $log) -ForegroundColor Red
    }

    return [pscustomobject]@{
        Demo    = $Name
        Status  = $status
        Size_KB = if ($ok) { Get-FirmwareSize -BuildDir $buildDir } else { 0 }
        Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        Log     = if ($ok) { '' } else { $log }
    }
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
$results = @()
$t0 = Get-Date

foreach ($t in $Targets) {
    $results += Invoke-OneDemoBuild -Name $t
}

# 恢复默认：不留「上一次构建的 demo」在 defaults 文件里误导人
Write-DemoConfig -Symbol 'APP_EDK60_DEMO_HELLO_BOARD'

$elapsed = (Get-Date) - $t0

Write-Host ''
Write-Host ('=' * 78) -ForegroundColor Cyan
Write-Host '构建结果汇总' -ForegroundColor Cyan
Write-Host ('=' * 78) -ForegroundColor Cyan
$results | Format-Table -AutoSize

if ($Size) {
    foreach ($r in $results) {
        Write-Host ("  {0,-20} {1,8} KB" -f $r.Demo, $r.Size_KB)
    }
}

$fail = @($results | Where-Object { $_.Status -ne 'OK' })
$color = if ($fail.Count -eq 0) { 'Green' } else { 'Red' }
Write-Host ("总计 {0} 个 demo：通过 {1}，失败 {2}，用时 {3:N1} 秒" -f `
        $results.Count, ($results.Count - $fail.Count), $fail.Count, $elapsed.TotalSeconds) -ForegroundColor $color

if ($fail.Count -gt 0) {
    Write-Host '失败清单：' -ForegroundColor Red
    foreach ($f in $fail) { Write-Host ("  - {0}  (日志: {1})" -f $f.Demo, $f.Log) -ForegroundColor Red }
    exit 1
}
