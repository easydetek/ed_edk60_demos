# VS Code + ESP-IDF 插件使用说明

> 本文档针对本仓库（EDK60 / ESP32-C6 多工程 Demo）与**本机已安装的 ESP-IDF v6.1**。
> 目标：在 VS Code 里完成「**用 menuconfig 选 demo → 编译 → 烧录 → 串口监视 → 调试**」全流程。

---

## 〇、前提：本机 ESP-IDF 安装位置

| 项 | 路径 |
|---|---|
| ESP-IDF | `C:\esp\v6.1\esp-idf`（v6.1.0） |
| 工具链根目录 | `C:\Espressif\tools` |
| IDF Python 虚拟环境 | `C:\Espressif\tools\python\v6.1\venv` |
| 官方激活脚本 | `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` |
| 安装方式 | Espressif Installation Manager（EIM，`~\.espressif\eim_gui`） |

仓库里已经放好 [.vscode/settings.json](../.vscode/settings.json)，路径就是按上表填的。
**换 IDF 版本或安装位置后，必须同步改这个文件。**

---

## 一、安装扩展

1. VS Code → 扩展市场 → 搜索并安装 **`ESP-IDF`**（发布者 *Espressif Systems*，扩展 ID `espressif.esp-idf-extension`）。
2. 建议同时安装 `C/C++`（`ms-vscode.cpptools`）以获得 IntelliSense。
3. 打开本仓库根目录（**不是** `main/` 或 `components/`，必须是含顶层 `CMakeLists.txt` 的那一层）：
   ```
   File → Open Folder → ed_edk60_demos
   ```

---

## 二、配置扩展（自动生效，一般无需手填）

`.vscode/settings.json` 已写入以下关键项，扩展启动时会自动读取：

| 设置项 | 值 | 说明 |
|---|---|---|
| `idf.espIdfPath` / `idf.espIdfPathWin` | `C:\esp\v6.1\esp-idf` | IDF 根目录 |
| `idf.toolsPath` / `idf.toolsPathWin` | `C:\Espressif\tools` | 工具链根目录 |
| `idf.pythonInstallPath` | `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe` | 必须指向 venv 里的 python |
| `idf.customExtraPaths` | 一长串工具路径 | cmake / ninja / riscv32-esp-elf / ccache / openocd 等 |
| `idf.customExtraVars` | `IDF_PATH` 等环境变量 | 与官方激活脚本等价 |
| `idf.adapterTargetName` | `esp32c6` | EDK60 的目标芯片 |
| `idf.flashType` | `UART` | 串口烧录 |

### 如果扩展仍提示 "ESP-IDF is not installed"

依次尝试：

1. 命令面板（`Ctrl+Shift+P`）→ **`ESP-IDF: Configure ESP-IDF Extension`** → 选 **Advanced** → 按下表手填：
   - `Enter ESP-IDF directory` → `C:\esp\v6.1\esp-idf`
   - `Enter ESP-IDF Tools directory` → `C:\Espressif\tools`
   - `Enter python executable path` → `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`
2. 或选 **EXPRESS** 让扩展自己装一套（会占用几个 GB，且与现有 v6.1 冲突，不推荐）。
3. 或选 **Use existing setup**：选 EIM 安装、IDF 版本选 `v6.1`。
   EIM 记录在 `C:\Espressif\tools\eim_idf.json`，其中的 `idfSelectedId` 会被 `settings.json` 的 `idf.currentSetup` 引用。

---

## 三、日常使用：三步走

### ① 选 demo（menuconfig）

| 方式 | 操作 |
|---|---|
| **图形化（推荐）** | 命令面板 `Ctrl+Shift+P` → **`ESP-IDF: SDK Configuration editor (menuconfig)`** → 在网页式界面里点开 **`EDK60 Demo` → `选择要编译的 Demo`** → 单选一个 → 点 **Save** → 顶部 **Build** 按钮 |
| **命令行** | 底部状态栏点 **⚙ (Open ESP-IDF Terminal)** → `idf.py menuconfig` |
| **直接改文件** | 编辑根目录 `sdkconfig`（或 `sdkconfig.defaults.esp32c6`），把 `CONFIG_APP_EDK60_DEMO_xxx=y` 那一行改掉 |

> menuconfig 保存后，`sdkconfig` 会变，**扩展会在下一次 Build 时自动重新配置**，无需手动 `set-target`。

**menuconfig 里能改什么**（详见 [README 第四节](../README.md#四menuconfig-选项)）：

- `EDK60 Demo` → 选择要编译的 Demo（15 选 1）
- `EDK60 板级配置` → 是否打印板卡横幅 / 引脚映射表 / 启动等待毫秒数
- `EDK60 Demo 参数` 系列 → Wi-Fi SSID 密码、SoftAP 参数、BLE 名称与扫描参数、雷达波特率、桥接模式、睡眠模式、NVS 命名空间等

### ② 编译 / 烧录 / 监视

| 功能 | 命令面板命令 | 快捷键 |
|---|---|---|
| 编译 | `ESP-IDF: Build your project` | `Ctrl+E B` |
| 选择串口 | `ESP-IDF: Select port to use` | `Ctrl+E P` |
| 烧录 | `ESP-IDF: Flash your project` | `Ctrl+E F` |
| 烧录+监视 | `ESP-IDF: Flash (UART) your project` | — |
| 串口监视器 | `ESP-IDF: Monitor your device` | `Ctrl+E M` |
| 全清理 | `ESP-IDF: Full clean` | — |
| 固件体积分析 | `ESP-IDF: Size analysis of the binaries` | — |
| 打开 IDF 终端 | `ESP-IDF: Open ESP-IDF Terminal` | — |

**本板的串口拓扑（重要）**：

```
USB-C ──直连── IO12/IO13 ── ESP32-C6 原生 USB Serial/JTAG
                              └─ 枚举为 USB 串行设备，可看日志（本工程默认当「第二输出口」）

H2 排针 ── IO16(D0/RX) + IO17(D1/TX) ── UART0 ── 本工程的控制台（可交互）
                              └─ 需外接 USB-TTL 串口板（3.3V 电平）
```

- **只想看日志**：插 USB-C，在 `ESP-IDF: Select port to use` 里选那个 USB 串行设备即可（任意波特率）。
- **想输入/交互（REPL、`uart_echo` 调试）**：接 USB-TTL 到 H2 的 D0/D1，波特率 115200，选对应 COM 口。
- **想烧录**：两种都行；USB 原生口通常更快更省事。

### ③ 调试（可选）

`.vscode/launch.json` 提供了两个配置：

| 配置名 | 适用 |
|---|---|
| `EDK60: 调试（USB Serial/JTAG）` | 用 **C6 内置 USB-JTAG**，不需要额外探针（本板 USB-C 已具备） |
| `EDK60: 调试（OpenOCD + GDB）` | 外接 FTDI/JTAG 探针的情形 |

> ⚠️ **使用 USB Serial/JTAG 调试前**，请先在 menuconfig 里把
> `Channel for console output` 改成 **USB Serial/JTAG**（默认为 UART0），否则控制台与调试器会抢外设。
> 改完记得把 `sdkconfig.defaults` 里的对应项也改掉，避免下次 `reset` 后被覆盖。

---

## 四、批量回归编译（命令行，非 GUI）

需要「一次性验证 15 个 demo 都能编过」时，用仓库自带的脚本（**本机默认禁止运行 ps1，必须带 `-ExecutionPolicy Bypass`**）：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -List
powershell -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -Demo blink,wifi_sta,radar_power
powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -Size
```

脚本会为每个 demo 生成 `build-<demo>/` 与 `sdkconfig.<demo>`，互不干扰；
脚本结束时会把 `sdkconfig.defaults.esp32c6` 复位成 `hello_board`，避免留下误导状态。

---

## 五、IntelliSense 不认头文件 / 报红波浪线

1. 先成功编译一次（生成 `build/compile_commands.json`）。
2. 命令面板 → `ESP-IDF: Add .vscode configuration folder`（若 `.vscode` 缺失）。
3. 命令面板 → `C/C++: Select IntelliSense Configuration...` → 选 `build/compile_commands.json`。
4. 仍不行时：`ESP-IDF: Full clean` → 重新 Build；或删除 `.vscode/.browse.*`。
5. 本项目源文件为 **UTF-8 带中文注释**，若出现中文乱码，确认：
   - `files.encoding: utf8`（已在 settings.json 设好）
   - 底部状态栏编码显示为 **UTF-8**

---

## 六、常见问题排查

| 现象 | 原因 / 处理 |
|---|---|
| 编译报 `[EDK60] 目标芯片必须是 esp32c6` | 目标芯片不是 C6。执行 `idf.py set-target esp32c6`，或把 `idf.adapterTargetName` 设为 `esp32c6` |
| 编译报 `[EDK60] 没有选中任何 Demo` | `sdkconfig` 里没有 `CONFIG_APP_EDK60_DEMO_*=y`。跑一次 menuconfig 选一个，或检查 `sdkconfig.defaults.esp32c6` |
| 串口监视器里中文变问号 | 监视器编码问题，改用 `idf.py monitor` 或把终端切 UTF-8；也可以用 `ESP-IDF: Open ESP-IDF Terminal` |
| 烧录时找不到串口 | 换一根**能传数据**的 USB 线（很多线只有供电）；确认驱动；Windows 设备管理器里看是否有「USB 串行设备」 |
| 插着 USB 但看不到日志 | 本工程默认控制台在 UART0(IO16/IO17)，USB 只是第二输出口；某些主机在枚举期会丢早期日志 → 把 `EDK60 板级配置 → 启动等待` 设为 500~2000ms |
| 上电直接进下载模式 | 上电时按住了 BOOT（IO9），属正常设计；松开后按 RESET 重启 |
| 扩展提示找不到 `riscv32-esp-elf-gcc` | `idf.customExtraPaths` 里的版本号与本机不一致（工具链升级后会变），按 `C:\Espressif\tools\riscv32-esp-elf\` 下的实际目录名更新 |
| 想彻底重来 | `idf.py fullclean` + 删除根目录 `sdkconfig`，然后重新 `set-target esp32c6` |

---

## 七、给后续维护者

- **不要**把 `build/`、`sdkconfig`、`build-*/`、`sdkconfig.<demo>` 提交进 Git（`.gitignore` 已处理）。
- `sdkconfig.defaults`、`sdkconfig.defaults.esp32c6`、`.vscode/settings.json` **应当**入库，它们是新环境的开箱配置。
- 升级 ESP-IDF 后必须复核三处：
  1. `.vscode/settings.json` 的所有路径（版本号会变）；
  2. `sdkconfig.defaults` 里的 Kconfig 符号是否仍存在（IDF 大版本会改名/删除）；
  3. `components/edk60_app/CMakeLists.txt` 顶部注释里记录的「组件早期展开读不到 CONFIG_*」这一行为是否仍成立。
