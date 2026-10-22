# EDK60 多工程 Demo（ESP32-C6）

[EDK60](https://github.com/easydetek/ed_edk60_demos) 开发板的 ESP-IDF 示例工程集合。
**一套工程、多个 demo，用 `menuconfig` 选择要编译运行哪一个。**

| 项目 | 内容 |
|---|---|
| 主控模组 | **ESP32-C6-MINI-1-N4**（RISC-V 单核 @160MHz，4MB Flash，无 PSRAM） |
| 开发框架 | **ESP-IDF v6.1** |
| 工程根目录 | 本仓库根目录（`CMakeLists.txt` 直接位于此处） |
| 引脚依据 | `docs/SCH_EDK60_VA.pdf`（EDK60_VA V1.0，2026-06-15）→ 整理为 [EDK60-pinmap.md](docs/EDK60-pinmap.md) |
| 串口能力 | C6 **原生 USB Serial/JTAG**（IO12/IO13），板上**无** USB 转串口芯片 |

---

## 一、目录结构

```
ed_edk60_demos/
├── CMakeLists.txt                  顶层工程（含目标芯片校验）
├── sdkconfig.defaults              工程默认编译配置（受版本控制）
├── sdkconfig.defaults.esp32c6      demo 选择（由 build_all.ps1 维护）
├── sdkconfig.defaults.radar_bridge demo 5.1 专用覆盖：关掉「USB 第二控制台」以保证数据干净
├── build_all.ps1                   批量编译脚本（逐 demo 独立 build 目录）
├── .vscode/                        VS Code + ESP-IDF 插件配置
│   ├── settings.json
│   └── launch.json
├── docs/
│   ├── SCH_EDK60_VA.pdf            原理图（V1.0）
│   ├── EDK60-pinmap.md             引脚定义表（含实测结论）★ 写代码前必读
│   ├── EDK60-pitfalls.md           开发避坑清单 + 排障速查表 ★ 遇到怪问题先看这个
│   ├── flash-guide-for-beginner.md 新手烧录指南（零基础、免安装）★ 发给客户的就是这份
│   ├── tutorial-edv151-vital-signs.md  EDV151 体征雷达打通全过程（开发者教程）
│   ├── vscode-setup.md             VS Code ESP-IDF 插件使用说明 ★
│   └── project/                    项目管理文档（章程/索引/风险/Gate 记录）
├── components/
│   ├── edk60_board/                板级支持包 BSP（引脚宏 + LED/按键/I2C/雷达电源/控制台）
│   │   ├── include/edk60_board.h   ★ 所有引脚宏都在这里，禁止在 demo 里硬编码
│   │   └── edk60_board.c
│   └── edk60_app/                  各 demo 实现 + 编译期分派
│       ├── include/edk60_demo.h    各 demo 的启动函数声明
│       └── src/
│           ├── demo_dispatch.c     menuconfig 选择 → 调用对应 demo
│           └── demo_get_started_*.c
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild           ★ menuconfig 菜单定义（demo 选择 + 可配参数）
    └── src/app_main.c              应用入口
```

---

## 二、快速开始（命令行）

### 1. 激活 ESP-IDF 环境

```powershell
# 方式 A：若已把 IDF 的 PowerShell 配置加进 profile，直接开新终端即可
idf.py --version

# 方式 B：手动注入（本机安装位置）
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
```

### 2. 选择 demo 并编译

```powershell
cd <本仓库根目录>

idf.py set-target esp32c6        # 首次使用（本工程只支持 esp32c6）
idf.py menuconfig                # → "EDK60 Demo" → "选择要编译的 Demo"
idf.py build
idf.py -p COM<x> flash monitor   # 烧录并看串口日志，Ctrl+] 退出
```

> **VS Code 用户请看 [docs/vscode-setup.md](docs/vscode-setup.md)**（用 ESP-IDF 插件图形化选择 demo / 编译 / 烧录 / 监视）。

### 3. 批量编译全部 demo（回归验证用）

本机默认禁止运行 PowerShell 脚本，请用带 `-ExecutionPolicy Bypass` 的方式调用：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_all.ps1              # 编译全部 demo
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -List        # 列出 demo 清单
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -Demo blink,button
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -Size        # 附带固件体积
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -Clean       # 先清理 build-<demo> 再编译
```

每个 demo 使用独立的 `build-<demo>/` 目录与 `sdkconfig.<demo>`，互不污染。

---

## 二·五、只想要「用上位机看雷达数据」？三步走

这是 adar_bridge（demo 5.1，EDV151 体征）与 adar_bridge_edv11p（demo 5.2，EDV11P 人数统计）的用途：把插在 **H3** 上的雷达模组的串口数据，
**原样字节透传**到 USB 串口，PC 上用任意上位机/串口助手就能看。**不做任何协议解析**。

**第 1 步：选 demo 并烧录**

```powershell
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -Demo radar_bridge
idf.py -p COM<x> flash
```
（或 VS Code：menuconfig → `EDK60 Demo` → `5.1 radar_bridge` → Build → Flash）

**第 2 步：看控制台，确认它找到了雷达**

> ⚠ **日志与数据是两条链路，别搞混**（这是本 demo 最容易踩的坑）：
>
> | 链路 | 承载什么 | 从哪输出 | 怎么看 |
> |---|---|---|---|
> | **UART0**（主控制台） | 启动横幅、识别过程、统计 | H2 排针的 **D1/TX(IO17)**（+D0/RX(IO16)） | USB-TTL 串口板，**115200** |
> | **USB Serial/JTAG** | **只剩雷达原始数据** | USB-C 口（IO12/IO13） | 任意串口助手，波特率随意 |
>
> **为什么不把日志也放 USB**：ESP32-C6 的 USB Serial/JTAG 只有**一个**外设，
> 在 PC 上就是一个 USB 串行设备。若日志也走 USB，它会与雷达数据写进同一个 FIFO，
> 上位机收到的就是「数据夹着中文日志」的混合流 —— 测数据时等于废掉。
> 因此本 demo 在 [sdkconfig.defaults.radar_bridge](sdkconfig.defaults.radar_bridge) 里
> **关掉了「控制台第二输出口 = USB」**，并且代码里有**编译期硬保护**：
> 若不慎把这个选项改回开启，`radar_bridge` 会直接编译失败并提示改法。
> （想临时在 USB 上看日志：勾选 demo 参数里的「USB 上也输出日志」，但要接受数据被污染。）
>
> JTAG 调试本身**不会**干扰数据 —— 它是同一外设的分时复用功能，不开调试器时不占链路。

接 USB-TTL 串口板到 H2 的 `D0/RX(IO16)`、`D1/TX(IO17)`，115200 打开。你会看到：

```
[radar_bridge] ===== 步骤 1/2：识别接收脚 =====
[radar_bridge]   IO14 : 收到   6432 字节 /  58 种字节  <- H4 的 T 脚（图纸标 IO14）
[radar_bridge]   IO15 : 收到      0 字节 /   0 种字节  <- H4 的 R 脚 / RADAR_PWR_EN
[radar_bridge]   IO2  : 收到      0 字节 /   0 种字节  <- H4 的 A 脚之一
...
[radar_bridge] ==> 接收脚识别结果：IO14（6432 字节）
[radar_bridge] ===== 步骤 2/2：识别波特率 =====
[radar_bridge]   921600 bps :   6432 字节 /  58 种字节
[radar_bridge]   115200 bps :     41 字节 /   9 种字节
[radar_bridge] ==> 波特率识别结果：921600 bps
```

> 这一步**同时会帮你定论 H-01**（原理图上 U4.EN 到底接 IO14 还是 IO15 —— 见
> [pinmap 第二节](docs/EDK60-pinmap.md)）：哪根脚在收数据，哪根就是模组的 TX 线。

**第 3 步：在 PC 上打开 USB 串口收数据**

设备管理器 → 端口 → 会多出一个 **「USB 串行设备」**（VID 303A）。用任意串口助手打开它：

- **波特率随便填**（USB CDC 不依赖波特率，数据是原速搬运的）；
- 应看到持续刷新的**原始字节**（可能是二进制，建议用 HEX 显示模式）；
- 在里面发几个字符 → 会**原样回显**，说明「PC → 板子」这条通路也通（本地回环自测，默认开）；
- **这里不该出现任何中文日志** —— 如果出现了，说明有人把控制台第二输出口改回了 USB（见第 2 步的说明）。

**如果第 2 步所有脚都收不到字节**，控制台会直接给出 6 条排查清单（模组没插好 / 没上电 /
使能脚不是 IO15 / T-R 接反 / 波特率不在候选列表 / 模组需先收配置命令）。按清单逐条走即可。

**看真实数据时建议关掉本地回环自测**（`menuconfig → EDK60 Demo 参数（雷达透传桥 5.1）→
本地回环自测`），避免回显混进雷达数据流。

---

## 三、Demo 清单

> **编译验证状态**：16/16 全部通过（ESP-IDF v6.1，`-Wall -Werror -Wextra`，0 error / 0 warning）。
> 各 demo 固件体积见 [编译验证报告](docs/project/gate-records/build-verification.md)。

### 1. 基础外设组（上位机、GPIO、UART、ADC）

| # | demo 名 | 目的 | 用到的板载资源 | 关键观察点 |
|---|---|---|---|---|
| 1.1 | `hello_board` | 板卡自检 + 串口心跳 | LED RX(IO8)/TX(IO9) | 上电看到 EDK60 横幅；两颗黄灯各闪一次；每 5 秒打印运行时长与空闲堆 |
| 1.2 | `blink` | 最小 GPIO 输出验证 | IO8、IO9 | RX 灯 1Hz 慢闪、TX 灯 5Hz 快闪 |
| 1.3 | `button` | 按键输入（轮询 vs 中断消抖） | BOOT 键 = IO9 | 按下/松开都打印；按住 ≥2s 标为长按；与 100ms 轮询结果对照 |
| 1.4 | `uart_echo` | 硬件 UART 收发 | UART1 重映射到 IO16(RX)/IO17(TX) | 从 IO16 发数据会原样回显（ASCII+HEX 双视图）；每 5 秒主动发心跳 |
| 1.5 | `adc_read` | ADC1 六通道采集与校准 | A0..A5 = IO0..IO5 | 每通道打印 raw / 校准 mV；末尾一行 CSV 可喂串口示波器 |

### 2. 无线组（Wi-Fi 6 / BLE 5）

| # | demo 名 | 目的 | 关键观察点 |
|---|---|---|---|
| 2.1 | `wifi_sta` | STA 扫描 / 连接 / 状态 | 打印 AP 表格（SSID/BSSID/信道/RSSI/加密）；连接过程事件齐全；断线自动重连 |
| 2.2 | `wifi_softap` | SoftAP + DHCP | 手机可连上并拿到 IP；实时打印站点上下线 |
| 2.3 | `ble_uart` | BLE 串口透传（NUS 兼容） | 手机 nRF Connect 能发现 `EDK60-BLE-UART` 并收发数据 |
| 2.4 | `ble_scan` | BLE 广播扫描 | 打印周边 BLE 设备，解析名字/UUID/厂商数据/iBeacon；每周期输出 RSSI 汇总表 |

> Wi-Fi 只支持 **2.4GHz**；ESP32-C6 无经典蓝牙，BLE demo 用 NimBLE 协议栈。

### 3. 板载互动组（针对 EDK60 特有电路）

| # | demo 名 | 目的 | 关键观察点 |
|---|---|---|---|
| 3.1 | `radar_power` | 3.3V_RADAR 使能时序 + 雷达 UART 观测 | 演示**关→开**一次电源复位；**丝印 "L" 的橙色 LED4 亮 = 雷达已上电**；未插模组会给出静默诊断提示；BOOT 键切换电源 |
| 3.2 | `led_sequence` | LEDC PWM 灯效编排 | 5 种灯效（交替/呼吸/追光/交叉呼吸/SOS），BOOT 键循环切换 |
| 3.3 | `i2c_scan` | I2C 总线扫描 | 打印地址网格 + 常见器件识别；**空板扫不到是正常现象**（板上无 I2C 器件） |

### 4. 低功耗与存储组

| # | demo 名 | 目的 | 关键观察点 |
|---|---|---|---|
| 4.1 | `deep_sleep` | 深睡 + 三种唤醒源 | 唤醒原因/累计次数打印；LED 闪次数编码唤醒源；EXT1 仅 IO0..IO7 可用 |
| 4.2 | `nvs_rw` | NVS 读写 / 统计 | 打印「本机第 N 次启动」、键值清单、分区使用统计 |
| 4.3 | `usb_serial_bridge` | **USB 转串口**（原生 USB ⇄ 雷达 UART 桥） | 插 USB 后 PC 出现 USB 串行设备；本地回环自测可验证 USB 通路。⚠ **实际为单向**：「雷达→USB」可用；「USB→雷达」因 UART1 的 TX 未布线而物理不通（详见下方硬件红线） |

### 5. 雷达透传组

| # | demo 名 | 目的 | 关键观察点 |
|---|---|---|---|
| 5.1 | `radar_bridge` | **雷达原始字节透传**：雷达模组 → UART → 原生 USB 串口 → PC 上位机（**不解析任何协议**） | ① 启动横幅与识别结果走控制台（H2 的 D0/D1，115200）；② 使能后丝印 "L" 橙灯亮 = 雷达已上电；③ PC 打开 USB 串行设备即可看到持续刷新的原始字节；④ 串口助手发字符会原样回显（本地回环自测）；⑤ 控制台每 10 秒打印双向字节统计 |
| 5.2 | `radar_bridge_edv11p` | **EDV11P 侧装雷达纯字节透传**：UART1(IO4/IO5, **921600**) ⇄ 原生 USB，**零协议注入、零 printf**，供网页版/桌面端直接解析轨迹与人数 | ① USB 流里**只有雷达数据**，无任何日志文本；② 上电后需主机下发一次 `SET 0x05=1` 才会开始上报（实测行为）；③ 配套：`release/track.html`（网页版）、`tools/edv11p_frame_check.py`（帧校验） |

> **5.1 与 4.3 的区别**：4.3 是「通用 USB↔UART 桥 + 调试视图」，5.1 是**面向雷达模组的开箱即用透传**，
> 多了三项关键能力：**① 自动识别接收脚与波特率**（解决本板 IO14/IO15 的读图歧义）；
> **② 雷达电源使能控制**；**③ 本地回环自测**（不接雷达也能先验证 USB 通路与上位机）。
> 两者都只做字节搬运、都不解析协议。
>
> **5.2（EDV11P）与 5.1（EDV151）的差异**：5.2 走 921600、**不发任何握手命令**（配置全部由上位机用 EDV11P 协议下发）、
> 并把日志彻底静默（连横幅都不打印），保证 USB 流里只有雷达帧；代价是排障要依赖 UART0 或 PC 侧抓包。

---

## 四、menuconfig 选项

进入 `idf.py menuconfig` → **EDK60 Demo**：

```
EDK60 Demo
  ┌ 选择要编译的 Demo                  ← choice，单选，对应上表 16 项
  ├ 启动时打印 EDK60 板卡横幅            (默认 y)
EDK60 板级配置
  ├ 启动时打印 EDK60 引脚映射表          (默认 y)
  └ 启动等待（毫秒）                      (默认 0，USB 枚举丢日志时可加)
EDK60 Demo 参数 / 参数（BLE）/ 参数（雷达·USB 桥）/ 参数（低功耗·存储）
EDK60 Demo 参数（雷达透传桥 5.1）        ← radar_bridge 专用参数组
                                          ← 各 demo 自己的可配参数（SSID、波特率、睡眠模式…）
```

### radar_bridge（5.1）的参数最常用这几项

`menuconfig → EDK60 Demo 参数（雷达透传桥 5.1）`：

| 选项 | 默认 | 说明 |
|---|---|---|
| 桥接用的 UART 端口 | UART1 | UART0 被控制台占用，不要选 |
| 雷达 TX → MCU 的接收脚（RX） | 14 | 若实测发现使能脚才是 IO15、而数据线是 IO15，改成 15 |
| MCU → 雷达 RX 的发送脚 | **-1（不用）** | 本板 H4 没引出这根线；改板后可填 |
| 雷达电源使能脚如何处理 | 占用 IO15 | 选「不占用」则把该脚留给其它用途（失去软开关） |
| 启动时使能雷达电源 | y | 点亮丝印 "L" 橙灯，让模组上电 |
| 雷达串口波特率 | 921600 | 按模组手册填；EDQ152 是 256000 |
| **启动时自动识别（接收脚 + 波特率）** | **y** | **推荐保持开启**，固件自己试出正确的脚与波特率 |
| 本地回环自测（USB 收什么回什么） | y | 不接雷达也能先验证 USB 通路；读真实数据时建议关 |
| 统计打印周期 | 10 秒 | 控制台打印双向字节数 |

`main/Kconfig.projbuild` 里还写明了**新增 demo 的五步流程**。

---

## 五、工程约定（改代码前请读）

1. **引脚一律取自 `components/edk60_board/include/edk60_board.h`**，禁止在 demo 里硬编码 GPIO 编号。
   该头文件含 `_Static_assert` 编译期校验，引脚写错会直接编译失败。
2. **demo 不放在 `main/`**：ESP-IDF 对组件做两次展开，`main` 在无源文件时会变成 INTERFACE 库导致注册冲突。
   实现放在 `components/edk60_app/src/`，由 `demo_dispatch.c` 按 Kconfig 分派。
3. **组件 CMakeLists 里不能写 `if(CONFIG_xxx)` 裁剪源文件**：组件在「早期展开」阶段解析，
   那时 `CONFIG_*` 还不是 CMake 变量（详见 `components/edk60_app/CMakeLists.txt` 的踩坑记录）。
   本工程因此一次性编译全部 demo，未选中的由链接器 `--gc-sections` 丢弃。
4. 新增 demo 四步：
   ① `main/Kconfig.projbuild` 加 `config`；② 加源文件并实现 `demo_<名>_start()`；
   ③ `components/edk60_app/include/edk60_demo.h` 加声明；④ `demo_dispatch.c` 加 `#elif` 分支；
   ⑤ `components/edk60_app/CMakeLists.txt` 的 `EDK60_DEMO_SRCS` 加一行。
5. 日志与注释用**中文**，文件头写清目的、硬件依据（引用原理图网络名）、观察点。
6. 编译选项含 `-Wall -Werror -Wextra`，**warning 会被当成错误**。

---

## 六、硬件红线（违反会出问题）

| 事项 | 说明 |
|---|---|
| **雷达引脚（已实测）** | **模组 TX→IO4、模组 RX←IO5、电源 EN=IO14、橙灯=IO15**。早期按图纸推断「EN=IO15、RX=IO14」**两个都错**，2026-10-15 上板实测推翻。**模组必须插 H3**（H4 的 T 与 EN 同网络会冲突）。**射频默认关闭**，必须发 `turn_on`；**增益过大会导致心率呼吸恒为 0**，需 `set_gain 0.5` + `set_boost 2`。详见 [pinmap 第二节](docs/EDK60-pinmap.md) 与 [避坑清单 G 节](docs/EDK60-pitfalls.md)。 |
| **雷达接口只有 RX** | IO15 被电源使能（兼指示灯）占用，且 ESP32-C6 的 **UART1 没有 IOMUX 默认引脚**（不显式布线就没有任何输出），因此 H4 上没有给雷达用的独立 TX —— **无法主动向雷达发命令**（`usb_serial_bridge` 因此是单向的）。要双向须改板另分一个空闲 GPIO。可热插拔：默认状态下 3.3V_RADAR 是关断的。 |
| **USB 是单一外设** | ESP32-C6 的 **USB Serial/JTAG 只有一个**（IO12/IO13），PC 上只呈现**一个** COM 口，CDC 数据与 JTAG 调试是它的分时复用。因此：**JTAG 不干扰数据**（不开调试器就不占链路），但**若把控制台日志也接到 USB，日志会与数据混进同一个 FIFO**。`radar_bridge` 已默认关掉该选项（见 [sdkconfig.defaults.radar_bridge](sdkconfig.defaults.radar_bridge)）并有编译期保护。 |
| **雷达电源使能脚存在读图歧义** | 原理图在 U4.EN 走线上同时标了 `RADAR_PWR_EN` 与 `IO14`，单页图纸无法唯一判定使能脚是 **IO15**（当前实现口径）还是 **IO14**。`radar_power` 内置自检日志、`radar_bridge` 有引脚自动识别，一次上板即可定论。详见 [pinmap 第二节](docs/EDK60-pinmap.md)。 |
| **IO8 / IO9 是 strapping** | 复位瞬间不要外接强驱动；IO9 同时是 BOOT 键，上电按住会进下载模式。 |
| **IO12 / IO13 被 USB 占用** | 原生 USB Serial/JTAG 固定引脚，拿去当 GPIO 会失去 USB 串口/调试能力。 |
| **ADC 只能用 ADC1** | C6 的 ADC2 不提供给用户程序；IO7 没有 ADC 功能；输入**不得超过 3.6V**。 |
| **深睡 EXT1 只支持 IO0..IO7** | BOOT 键（IO9）不在其列，按键唤醒需另一条 API 且可能不被支持。 |
| **板上有 3 颗常亮 LED** | ON(3.3V)、L(3.3V)、RADAR(3.3V_RADAR) 不受程序控制，会抬高静态功耗。 |

---

## 七、文档索引

| 文档 | 用途 |
|---|---|
| [docs/EDK60-pinmap.md](docs/EDK60-pinmap.md) | 引脚定义表、雷达电源控制要点、电源树、避让清单、待实测确认项 |
| [docs/vscode-setup.md](docs/vscode-setup.md) | VS Code + ESP-IDF 插件：选 demo、编译、烧录、串口监视、调试 |
| [docs/project/project-charter.md](docs/project/project-charter.md) | 项目章程（目标 / 范围 / 成功标准 / 治理） |
| [docs/project/deliverable-index.md](docs/project/deliverable-index.md) | 交付物索引（编号 / 版本 / 状态 / 责任人） |
| [docs/project/risk-register.md](docs/project/risk-register.md) | 风险登记册（含硬件待实测项） |
| [docs/project/gate-records/](docs/project/gate-records/) | 阶段 Gate 评审记录 |
| [docs/recipe-edv11p-person-tracking.md](docs/recipe-edv11p-person-tracking.md) | **EDV11P 侧装人数统计实施指南**（接线 / 烧录 / 协议 / 调参 / 排障） |
| [docs/case-edv11p-person-tracking.md](docs/case-edv11p-person-tracking.md) | EDV11P 应用案例（含 30 分钟实测数据与轨迹图） |
| [docs/project/edv11p/handover.md](docs/project/edv11p/handover.md) | EDV11P demo 结项交接包（交付物 / 未闭合项 / 复现命令） |

---

## 八、许可

MIT（见 [LICENSE](LICENSE)）。
