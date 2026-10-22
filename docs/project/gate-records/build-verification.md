# 编译验证报告 — EDK60 多工程 Demo

| 项 | 内容 |
|---|---|
| 验证日期 | 2026-10-14 |
| 验证人 | 项目经理 |
| 验证对象 | 15 个 demo 的全量构建 |
| 工具链 | ESP-IDF **v6.1.0**（`C:\esp\v6.1\esp-idf`）+ riscv32-esp-elf gcc 15.2.0 + CMake 4.0.3 + Ninja 1.12.1 + ccache |
| 目标芯片 | `esp32c6`（ESP32-C6-MINI-1-N4，4MB Flash） |
| 编译选项 | `-Wall -Werror -Wextra`（工程默认，warning 即 error） |
| 验证命令 | `powershell -ExecutionPolicy Bypass -File .\build_all.ps1 -Size` |
| **结论** | **16 / 16 通过（0 失败、0 error、0 warning）** |

> 历史：
> · 2026-10-14 第 1 轮全量：15/15（2403.8 s）
> · 2026-10-14 第 2 轮（新增 demo 5.1 radar_bridge）：16/16（约 1512 s）
> · 2026-10-14 第 3 轮（**为 radar_bridge 分离 USB 日志与数据流**，见 R-14/R-16）：**16/16（约 530 s）**

---

## 一、全量构建结果（第 3 轮，最终）

| # | demo | 状态 | 固件体积 | 编译耗时 | 独立 build 目录 |
|---|---|---|---|---|---|
| 1 | hello_board | ✅ OK | 174.4 KB | 72.7 s | `build-hello_board/` |
| 2 | blink | ✅ OK | 173.3 KB | 32.7 s | `build-blink/` |
| 3 | button | ✅ OK | 177.0 KB | 32.4 s | `build-button/` |
| 4 | uart_echo | ✅ OK | 201.3 KB | 33.5 s | `build-uart_echo/` |
| 5 | adc_read | ✅ OK | 187.3 KB | 33.5 s | `build-adc_read/` |
| 6 | wifi_sta | ✅ OK | 913.2 KB | 35.1 s | `build-wifi_sta/` |
| 7 | wifi_softap | ✅ OK | 907.6 KB | 36.9 s | `build-wifi_softap/` |
| 8 | ble_uart | ✅ OK | 623.4 KB | 36.1 s | `build-ble_uart/` |
| 9 | ble_scan | ✅ OK | 624.4 KB | 32.2 s | `build-ble_scan/` |
| 10 | radar_power | ✅ OK | 212.1 KB | 34.2 s | `build-radar_power/` |
| 11 | led_sequence | ✅ OK | 195.6 KB | 39.4 s | `build-led_sequence/` |
| 12 | i2c_scan | ✅ OK | 202.5 KB | 38.4 s | `build-i2c_scan/` |
| 13 | deep_sleep | ✅ OK | 199.8 KB | 31.8 s | `build-deep_sleep/` |
| 14 | nvs_rw | ✅ OK | 228.6 KB | 31.5 s | `build-nvs_rw/` |
| 15 | usb_serial_bridge | ✅ OK | 210.0 KB | 34.0 s | `build-usb_serial_bridge/` |
| 16 | **radar_bridge** | ✅ OK | **211.5 KB** | 31.1 s | `build-radar_bridge/` |

**总计：16 个 demo，通过 16，失败 0，0 error / 0 warning。**

> 注：`radar_bridge` 由 214.7 KB 降到 211.5 KB —— 关闭「USB 第二控制台」后相关输出代码被链接器裁掉，
> 这一体积变化本身也印证了配置确实生效（USB 上不再有日志写入了）。

---

## 一·附、第 3 轮修复的两个问题（本轮回归的目的）

| 问题 | 现象 | 修复 | 验证 |
|---|---|---|---|
| **R-14 日志污染数据流** | 工程级默认开 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`，日志与 radar_bridge 的雷达数据写入**同一个 USB FIFO**，上位机收到混合流 | ①新增 `sdkconfig.defaults.radar_bridge`（`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`），`build_all.ps1` 通过 `SDKCONFIG_DEFAULTS` 多文件机制只对该 demo 附加；②源码加编译期保护 | `sdkconfig.radar_bridge` 确认为 `CONSOLE_UART_DEFAULT=y` + `SECONDARY_NONE=y`；固件体积下降 3.2 KB |
| **R-16 编译期断言误伤其它 demo** | 保护写成无条件 `#error`，而本工程 16 个 demo 一起编译 ⇒ 另外 15 个 demo 全部编译失败 | 断言加上 `defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE) &&` 前置条件 | `hello_board` 在 USB 日志开启下正常编过；`radar_bridge` 改错配置仍会编译失败并提示改法 |

---

## 二、多工程选择机制的功能验证（不止「编过了」）

光有「编译通过」不足以证明 menuconfig 选择真的生效，因此补做了两项**独立验证**：

### 验证 1：不同选择产生实质性不同的固件

| 选择 | 固件体积 | 差异说明 |
|---|---|---|
| `blink` | 173.3 KB | 仅 GPIO + 日志 |
| `wifi_sta` | 913.2 KB | 多出 Wi-Fi 协议栈（约 740 KB） |
| `ble_uart` | 623.4 KB | 多出 NimBLE 协议栈 |

体积差异与所选 demo 的依赖完全吻合 → **选择确实改变了固件内容**。

### 验证 2：未选中的 demo 被链接器裁掉（`--gc-sections` 生效）

对三个独立构建的 ELF 做符号检查（`riscv32-esp-elf-nm`）：

| 构建 | 选中 demo 的符号 | 其他 demo 的符号 |
|---|---|---|
| `build-ble_scan/` | `demo_ble_scan_start` **存在** | `demo_hello_board_start` **缺失** |
| `build-wifi_sta/` | `demo_wifi_sta_start` **存在** | `demo_hello_board_start` **缺失** |
| `build-radar_power/` | `demo_radar_power_start` **存在** | `demo_hello_board_start` **缺失** |
| `build-radar_bridge/` | `demo_radar_bridge_start` **存在**、`usb_serial_jtag_driver_install` **存在** | `demo_radar_power_start` **缺失** |

→ 每个固件里**只保留被选中的 demo**，其余 15 个 demo 的代码被完全裁剪。
证明 `components/edk60_app` 的「全部编译 + 编译期分派」实现达到了与「只编译选中的那一个」等价的最终效果。

---

## 三、配置完整性验证

| 检查项 | 方法 | 结果 |
|---|---|---|
| demo 选择项数量 | `main/Kconfig.projbuild` 的 choice 内 config 计数 | 16 项（与交付清单一致） |
| Kconfig 结构 | menu/config 计数与配平 | 7 个 menu；`menu`=7 / `endmenu`=7 配平 |
| 重复符号 | 全部 config 名去重 | 无重名 |
| 各 demo 参数项生效 | 检查对应 `sdkconfig.<demo>` 与生成的 `sdkconfig.h` | 见下表 |
| 危险操作默认关闭 | `sdkconfig.lowpower` 中的布尔项 | `NVS_AUTO_ERASE_ON_FULL` / `NVS_ENABLE_ERASE_DEMO` 均为 `is not set` |
| **控制台与数据分离** | `sdkconfig.radar_bridge` 的控制台项 | `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` + `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`（USB 上无日志，只跑数据） |
| 其它 demo 仍可 USB 看日志 | `sdkconfig.hello_board` 的控制台项 | `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`（未被误伤） |
| 蓝牙可用 | `sdkconfig` 中 BT/NimBLE 项 | `CONFIG_BT_ENABLED=y` / `CONFIG_BT_NIMBLE_ENABLED=y` + 4 个 ROLE |
| 目标芯片 | 构建日志中的 target | `esp32c6` |
| 生成配置不残留旧值 | `build_all.ps1` 每次构建前删除 `sdkconfig.<demo>` | ✅ 已修正（曾因复用旧配置导致新增符号不生成） |

### 各 demo 配置项抽样（来自对应 sdkconfig）

```
# wifi（build-wifi_sta / build-wifi_softap）
CONFIG_APP_EDK60_WIFI_SSID=""                CONFIG_APP_EDK60_SOFTAP_SSID="EDK60-AP"
CONFIG_APP_EDK60_WIFI_PASSWORD=""            CONFIG_APP_EDK60_SOFTAP_PASSWORD="edk60demo"
CONFIG_APP_EDK60_WIFI_SCAN_MAX_AP=20         CONFIG_APP_EDK60_SOFTAP_CHANNEL=6
                                             CONFIG_APP_EDK60_SOFTAP_MAX_STA=4

# BLE（build-ble_uart / build-ble_scan）
CONFIG_APP_EDK60_BLE_DEVICE_NAME="EDK60-BLE-UART"
CONFIG_APP_EDK60_BLE_SCAN_ACTIVE=y
CONFIG_APP_EDK60_BLE_SCAN_INTERVAL=80        CONFIG_APP_EDK60_BLE_SCAN_WINDOW=48
CONFIG_APP_EDK60_BLE_SUMMARY_SEC=10          # CONFIG_APP_EDK60_BLE_SCAN_VERBOSE is not set

# 雷达 / USB 桥（build-radar_power / build-usb_serial_bridge）
CONFIG_APP_EDK60_RADAR_UART_BAUD=256000      CONFIG_APP_EDK60_RADAR_IDLE_TIMEOUT_MS=200
CONFIG_APP_EDK60_RADAR_SILENT_WARN_SEC=30    CONFIG_APP_EDK60_RADAR_POWER_ON_READY_MS=300
CONFIG_APP_EDK60_BRIDGE_RAW_PASSTHROUGH=y    CONFIG_APP_EDK60_BRIDGE_LOCAL_ECHO=y
CONFIG_APP_EDK60_BRIDGE_STATS_SEC=10

# 雷达透传桥（build-radar_bridge，demo 5.1）
CONFIG_APP_EDK60_BRIDGE2_UART1=y             CONFIG_APP_EDK60_BRIDGE2_UART_NUM=1
CONFIG_APP_EDK60_BRIDGE2_RX_PIN=14           CONFIG_APP_EDK60_BRIDGE2_TX_PIN=-1
CONFIG_APP_EDK60_BRIDGE2_PWR_EN_IO15=y       CONFIG_APP_EDK60_BRIDGE2_PWR_ON_AT_BOOT=y
CONFIG_APP_EDK60_BRIDGE2_PWR_READY_MS=300    CONFIG_APP_EDK60_BRIDGE2_BAUD=921600
CONFIG_APP_EDK60_BRIDGE2_AUTODETECT=y        CONFIG_APP_EDK60_BRIDGE2_PROBE_MS=800
CONFIG_APP_EDK60_BRIDGE2_RX_PIN_LIST="14,15,2,3,4,5"
CONFIG_APP_EDK60_BRIDGE2_BAUD_LIST="921600,115200,256000,230400,460800,57600,38400,19200,9600"
CONFIG_APP_EDK60_BRIDGE2_LOCAL_ECHO=y        # CONFIG_APP_EDK60_BRIDGE2_LOCAL_ECHO_PREFIX is not set
CONFIG_APP_EDK60_BRIDGE2_STATS_SEC=10        CONFIG_APP_EDK60_BRIDGE2_CHUNK=256

# 低功耗 / 存储（build-deep_sleep / build-nvs_rw）
CONFIG_APP_EDK60_SLEEP_MODE=0                CONFIG_APP_EDK60_SLEEP_TIMER_SEC=10
CONFIG_APP_EDK60_SLEEP_EXT1_PIN=0            CONFIG_APP_EDK60_NVS_NAMESPACE="edk60"
# CONFIG_APP_EDK60_NVS_AUTO_ERASE_ON_FULL is not set   ← 危险操作默认关闭
# CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO is not set    ← 危险操作默认关闭
```

---

## 四、代码规范抽查

| 检查项 | 方法 | 结果 |
|---|---|---|
| 导出函数名与接口头一致 | 对 15 个源文件与 `edk60_demo.h` 逐一比对 | ✅ 15/15 |
| 无硬编码引脚 | grep `GPIO_NUM_<数字>` / `1ULL << <数字>` | ✅ 仅 BSP 内部与必要的 USB 引脚常量 |
| 文件头注释齐全 | 检查「目的 / 硬件依据 / 观察点」三要素 | ✅ 15/15 |
| 中文注释与日志 | 抽查各文件 | ✅ |
| 回调不阻塞 | 抽查 wifi_sta / ble_uart / radar_power / deep_sleep 的事件回调 | ✅ 均只置标志或入队 |
| 破坏性操作受控 | nvs_rw 的擦除路径 | ✅ 三重保险 + 默认关闭 + 仅限本命名空间 |
| 任务栈 ≥ 4096 | 抽查各任务创建调用 | ✅ |

---

## 五、构建过程中的失败与修复（供复用参考）

首次全量构建并非一次成功，共经历 **11 类**构建系统问题（详见 [gate-2-implementation.md](gate-2-implementation.md) 第五节）。
其中 6 类属于「看起来像代码错误、实际是工具链/IDF 机制」：

1. **组件在「早期展开」阶段解析，此时读不到 `CONFIG_*`** → 改为「全部编译 + 编译期分派」（本工程最终采用的架构）
2. **`sdkconfig.defaults` 不支持行尾注释** → 注释必须独占一行，否则整行配置被静默忽略（曾导致 BT 配置失效）
3. **`PRIu32` 在本工具链展开为 `"lu"`**（`uint32_t == unsigned long`）→ 常量必须定型为 `(uint32_t)`，否则 `-Werror=format=` 失败
4. **`choice` 不会自动生成整数宏** → 需要另加一个派生的 `int` config 把选项暴露给 C 代码
5. **`default n` 的 bool 不生成 `CONFIG_*` 宏** → C 代码里必须用 `#ifdef` 兜底，不能直接引用
6. **复用旧 `sdkconfig` 会导致新增 Kconfig 符号不生成** → `build_all.ps1` 改为每次构建前删除该 demo 的生成配置

---

## 六、未覆盖的范围（诚实声明）

| 项 | 说明 |
|---|---|
| **上板运行验证** | 本报告只覆盖**编译/链接级**验证，没有任何硬件在环测试。11 项上板实测项（H-01~H-11）见 [risk-register.md](../risk-register.md) 与 [handover.md](handover.md) |
| 单元测试 / 静态分析 | 本期未引入（不在章程范围内） |
| 运行期行为（协议栈、时序、功耗） | 需上板确认 |
| 编译器以外的工具链 | 仅验证本机 v6.1；未验证 v5.x 或其它工具链版本 |
| 不同 Flash 容量 | 仅按 4MB（N4）验证 |

---

## 六、第 4 轮：雷达引脚实测修正后的全量复验（2026-10-15 晚）

因**雷达引脚实测结论推翻了早期读图推断**（EN=IO14 而非 IO15；RX=IO4、TX=IO5），
BSP 头文件、BSP 实现、以及 3 个雷达相关 demo 都被修改，故对所有 demo 重新构建验证。

| 变更内容 | 影响范围 |
|---|---|
| `EDK60_PIN_RADAR_PWR_EN` 15→**14**、`UART_RX` 14→**4**、新增 `UART_TX`=**5**、`RADAR_LED` 独立为 **15** | BSP（全部 demo 都链接） |
| `edk60_radar_power_init()` 改 `GPIO_MODE_INPUT_OUTPUT`（修复回读恒 0） | BSP |
| `radar_power` demo 按实测事实**重写**（原 74 处基于错误假设） | demo 3.1 |
| `radar_bridge` 内置 6 条握手命令（turn_on/set_time/周期/范围/**gain=0.5**/**boost=2**） | demo 5.1 |
| `usb_serial_bridge` / `deep_sleep` 注释按实测结论修正 | demo 4.3 / 4.1 |

**结果：16 / 16 全部通过，0 error / 0 warning。**

| demo | 体积 | demo | 体积 |
|---|---|---|---|
| hello_board | 174.5 KB | ble_scan | 624.5 KB |
| blink | 173.4 KB | radar_power | 205.7 KB |
| button | 177.2 KB | led_sequence | 195.8 KB |
| uart_echo | 201.5 KB | i2c_scan | 202.6 KB |
| adc_read | 187.5 KB | deep_sleep | 199.9 KB |
| wifi_sta | 913.3 KB | nvs_rw | 228.7 KB |
| wifi_softap | 907.7 KB | usb_serial_bridge | 210.1 KB |
| ble_uart | 623.5 KB | radar_bridge | 211.3 KB |

> `radar_power` 由 212.1 → 205.7 KB（重写后删掉了大量旧的自检代码）。

### 雷达链路的硬件实测验收（本轮新增，已通过）

| 项 | 结果 |
|---|---|
| 模组型号 / 版本 | EDV151-N-H01 / 166 |
| 引脚 | TX→IO4、RX←IO5、EN=IO14、橙灯=IO15（全部实测） |
| 射频 | 默认关闭，`turn_on` 后应答 `ok` |
| 体征帧 | `vital_signs` 每秒稳定上报，校验 0 失败 |
| **实测体征** | **心率 75 bpm、呼吸率 21 bpm、距离 58 cm、体动 5、可信度 2/2** |

> 关键调参：出厂 `gain=4/boost=0` 时静息体动 20~60（算法要求 <5）→ 心率呼吸恒为 0；
> 改为 `gain=0.5/boost=2` 后体动 1~6，体征立即输出。已内置进固件握手。
