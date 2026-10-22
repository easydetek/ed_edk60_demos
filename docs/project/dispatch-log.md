# 任务分派记录 — EDK60 多工程 Demo

| 项 | 内容 |
|---|---|
| 项目 | EDK60 多工程 Demo |
| 记录人 | 项目经理 |
| 建立日期 | 2026-10-14 |
| 最近更新 | 2026-10-14 |

---

## 一、分派原则

- **自包含**：子代理看不到主对话历史，每个任务说明都携带完整背景、构建环境、硬件事实、代码规范、验收标准。
- **并行优先**：无依赖任务同批启动（4 个子代理，各负责 2 个 demo）。
- **冲突隔离**：
  - 每个子代理使用独立 `build-<组名>` 目录；
  - 写入范围明确划定（各自的 2 个 demo 源文件 + `main/Kconfig.projbuild` **末尾追加**自己的 menu）；
  - 禁止修改 `CMakeLists.txt` / BSP / 其他 demo / `app_main.c`，缺依赖须上报而非自行添加。
- **验收可检查**：要求「自身文件编译 0 error（本工程 `-Wall -Werror -Wextra`）+ 贴出编译证据 + sdkconfig 出现新配置项 + 无硬编码引脚」。

---

## 二、主对话（项目经理）自行完成的部分

| # | 任务 | 产出 | 状态 |
|---|---|---|---|
| P-01 | 勘查仓库与原理图，确认硬件事实 | 引脚/电源/雷达/ADC 结论 | ✅ |
| P-02 | 核查工具链与 IDF 版本 | ESP-IDF v6.1 + Python 3.11.15 可用性结论 | ✅ |
| P-03 | 核实技术疑点（USB 转串口可行性、深睡唤醒源、ADC 通道、IO 占用） | 基于 IDF 头文件与 datasheet 的结论 | ✅ |
| P-04 | 搭建工程骨架 | `CMakeLists.txt` / `main/Kconfig.projbuild` / `sdkconfig.defaults` / `build_all.ps1` | ✅ |
| P-05 | 实现板级支持包 `edk60_board` | 引脚宏 + LED/按键/I2C/雷达电源/控制台/板卡信息 | ✅ |
| P-06 | 实现基础外设组 5 个 demo | hello_board / blink / button / uart_echo / adc_read | ✅ |
| P-07 | 实现板载互动组 2 个 demo | led_sequence / i2c_scan | ✅ |
| P-08 | **P0 勘误**：雷达电源控制极性（R2 由「10k 上拉到 5V」更正为「10k 下拉到 GND」），并同步 BSP / demo 规格 / 全部文档 / 风险登记册 | CR-06 | ✅ |
| P-09 | 修复 `sdkconfig.defaults` 行尾注释导致 BT 配置整行失效的问题；补 `CONFIG_BT_ENABLED` / `CONFIG_BT_NIMBLE_ENABLED` 等 8 项 | BLE 组可编译 | ✅ |
| P-10 | 定位并下发「`PRIu32` 在本工具链展开为 `%lu`」的根因（affected: nvs_rw） | 精确修复指引 | ✅ |
| P-11 | 编写文档（README / pinmap / vscode-setup / 章程 / 风险 / 索引 / Gate 记录 / 交接包） | DOC-001~007, REC-001~004 | ✅ |
| P-12 | 解决 IDF 6.1 组件展开机制导致的构建失败（5 类坑，见风险登记册「经验沉淀」） | 骨架可编译 | ✅ |

---

## 三、子代理分派清单与结果

| # | 角色 | 任务 | 交付物 | 构建目录 | 状态 |
|---|---|---|---|---|---|
| D-01 | 嵌入式/软件工程师 | Wi-Fi 组：2.1 `wifi_sta` + 2.2 `wifi_softap` | `demo_get_started_wifi_sta.c`（931 行）、`demo_get_started_wifi_softap.c`（548 行）、Kconfig 追加 | `build-wifi` | ✅ 已交付 |
| D-02 | 嵌入式/软件工程师 | BLE 组：2.3 `ble_uart` + 2.4 `ble_scan` | `demo_get_started_ble_uart.c`（896 行）、`demo_get_started_ble_scan.c`（964 行）、Kconfig 追加 | `build-ble` | 🔄 收尾中 |
| D-03 | 嵌入式 + 硬件工程师 | 板载/USB：3.1 `radar_power` + 4.3 `usb_serial_bridge` | `demo_get_started_radar_power.c`（463 行）、`demo_get_started_usb_serial_bridge.c`（461 行）、Kconfig 追加 | `build-radar` | 🔄 收尾中 |
| D-04 | 嵌入式/软件工程师 | 低功耗/存储：4.1 `deep_sleep` + 4.2 `nvs_rw` | `demo_get_started_deep_sleep.c`（929 行）、`demo_get_started_nvs_rw.c`（1107 行）、Kconfig 追加 | `build-lowpower` | 🔄 收尾中 |

### 分派时统一注入的上下文（摘要）

| 类别 | 内容 |
|---|---|
| 工程位置 | `C:\Users\fuhua\OneDrive\easydetek\0.project\20261014-ed_edk60_demos\ed_edk60_demos` |
| 构建环境 | ESP-IDF v6.1 + 激活脚本路径 + `-ExecutionPolicy Bypass` 要求 + 独立 build 目录 |
| 重要设计约束 | 「15 个 demo 源文件一起编译」⇒ 各自代码必须独立编译通过 |
| 硬件事实 | LED=IO8/IO9 低有效；BOOT=IO9（strapping）；雷达 RX=IO14、电源 EN=IO15；I²C=IO6/IO7；ADC1=IO0..IO5；USB=IO12/IO13 原生；UART0 控制台=IO16/IO17 |
| 代码规范 | 中文注释；文件头写「目的/硬件依据/观察点」；禁止硬编码引脚；`-Wall -Werror` |
| 写入范围 | 仅限指定的 2 个源文件 + `main/Kconfig.projbuild` 末尾追加 |
| 验收标准 | 导出函数名与 `edk60_demo.h` 一致 + 自身文件 0 error（附证据）+ sdkconfig 出现新项 + 无硬编码引脚 + 中文注释齐全 |
| 危险操作红线 | 禁止删除/擦除类破坏性操作；NVS erase 默认关闭且需显式 Kconfig 开关 |

---

## 四、跨代理冲突与处置记录

| 冲突点 | 现象 | 处置 | 结果 |
|---|---|---|---|
| 4 个子代理都改 `main/Kconfig.projbuild` | 各自「整体重写」导致先写者内容被覆盖（lost update） | 要求改为「仅在末尾追加」；项目经理在收齐后做全文一致性检查（6 个 menu、45 个 config 全部在位） | ✅ 已核实无丢失 |
| 4 个子代理都要跑 `idf.py build`，且默认共用仓库根 `sdkconfig` | 并发读写同一 sdkconfig，存在竞态 | 统一改为 `-D SDKCONFIG=<仓库根>\sdkconfig.<组名>`；项目经理的集成验证改在 `%TEMP%` 隔离副本中进行 | ✅ 已消除 |
| 多个 demo 在同一组件内编译 | 任一文件编译失败会连带阻塞全体（15 个一起编译的设计取舍） | 每个任务书强调「必须独立编译通过」；集成阶段由项目经理统一收口 | ⚠️ 设计取舍，已记录 |
| 项目经理早期同步子目录做集成编译 | 子代理后续写的文件未进快照，导致「报错行号与文件内容不符」的误判，一度把过期快照的错误转给 owner 返工 | 已向相关子代理澄清并致歉；后续统一「子代理发布完成→项目经理一次性同步→单次集成编译」 | ✅ 已纠正 |
| 雷达电源极性（**P0**） | 规格下发错误，radar_power 子代理需返工 | 项目经理重新逐层读图确认后全面更正并下发更正说明 | ✅ 已返工完成 |

---

## 五、验收核验结果

| # | 交付物 | 函数名匹配 | 自身文件编译 | Kconfig 生效 | 无硬编码引脚 | 硬件结论进注释 | 结论 |
|---|---|---|---|---|---|---|---|
| D-01 | wifi_sta / wifi_softap | ✅ | ✅ 0 error 0 warning | ✅ 7 项 | ✅ | ✅ | **通过** |
| D-02 | ble_uart / ble_scan | ✅ | ✅（含 1 次自身返工） | ✅ 6 项 | ✅ | ✅ | 待最终确认 |
| D-03 | radar_power / usb_serial_bridge | ✅ | ✅ | ✅ 7 项 | ✅ | ✅ 含极性更正后的三处结论 | 待最终确认 |
| D-04 | deep_sleep / nvs_rw | ✅ | 🔄 修复中（`PRIu32` 类型 + Kconfig 布尔坑） | ✅ 6 项 | ✅ | ✅ | 待最终确认 |

> 集成编译（15/15 链接成功）由项目经理在隔离副本中统一执行，结果见 [build-verification.md](gate-records/build-verification.md)。

---

## 六、追加交付（用户二次需求）

### 6.1 需求
用户插入 EDV151 雷达模组后提出：**只需把数据透传给 PC 上位机，不需要解析协议**。
链路要求：`雷达 ↔ ESP32-C6 UART ↔ ESP32-C6 USB Serial COM ↔ PC`。

### 6.2 交付物
| # | 交付物 | 行数 | 说明 |
|---|---|---|---|
| 1 | `components/edk60_app/src/demo_get_started_radar_bridge.c` | 738 | Demo 5.1：纯字节透传 + 接收脚/波特率自动识别 + 雷达电源控制 + 本地回环自测 |
| 2 | `main/Kconfig.projbuild`（追加 `EDK60 Demo 参数（雷达透传桥 5.1）` menu） | +160 | 12 项参数（UART 端口、RX/TX 引脚、电源使能脚、开机使能、波特率链、候选引脚链、探测时长、回环自测、日志开关、统计周期、搬运块） |
| 3 | `components/edk60_app/include/edk60_demo.h` / `src/demo_dispatch.c` / `CMakeLists.txt` | — | 菜单项、声明、分派、源列表同步 |
| 4 | `build_all.ps1` | — | 加入 `radar_bridge`；并修正「复用旧 sdkconfig 导致新增符号不生成」的隐患 |
| 5 | `README.md` / `docs/EDK60-pinmap.md` | — | 新增「只想要上位机看雷达数据？三步走」章节与自动识别说明 |

### 6.3 设计要点
- **引脚歧义工程化**：本板 `U4.EN` 的网络标号存在歧义（`RADAR_PWR_EN` vs `IO14`），
  本 demo 不猜——启动时把候选脚（IO14/IO15/IO2..IO5）**依次配成 UART RX**（仅输入，不驱动外部电路），
  统计各脚收到的字节数，**哪根在收数据哪根就是数据线**。这一步同时把风险 R-01/H-01 定论。
- **波特率识别**：ESP32-C6 **没有**硬件波特率自动检测（那是老 ESP32 的 `uart_baudrate_detect`），
  因此用软件穷举 + 启发式评分（字节数优先、字节种类数次之），并打印全部候选的得分供人工确认。
- **控制台与数据分离**：日志走 UART0（H2 的 D0/D1），USB 上跑**干净的数据**，
  互不污染；仅在排查「USB 完全没反应」时才需要打开「USB 上也输出日志」。
- **本地回环自测**：不接雷达模组也能先验证 USB 通路与上位机是否正常（默认开）。
- **诚实标注硬件限制**：本板 H4 未引出 MCU→模组 的数据线，因此默认只有「雷达 → PC」一个方向；
  若改板引出空闲 GPIO，把「MCU → 雷达 RX 的发送脚」填上即可打通反向。

### 6.4 本次踩到并已修复的两个新 Kconfig 坑
1. **`choice` 不生成整数宏** → 另加派生 `int` config（`APP_EDK60_BRIDGE2_UART_NUM`）把选项暴露给 C 代码；
2. **`default n` 的 bool 不生成 `CONFIG_*` 宏** → C 代码里用 `#ifdef` 兜底，不能直接引用。

### 6.5 验证
- 单 demo 构建：`radar_bridge` ✅ 通过，固件 211.5 KB；
- ELF 符号核对：`demo_radar_bridge_start` 存在、`demo_radar_power_start` 缺失（裁剪生效）；
  且 `usb_serial_jtag_driver_install/read_bytes/write_bytes` 均已链接进固件；
- **全量回归：16/16 通过，0 error / 0 warning**（见 [build-verification.md](gate-records/build-verification.md)）。

### 6.6 用户追问引发的关键修正：USB 日志会污染数据流（R-14）

用户问「USB 接口功能不会收到 USB Serial COM 日志和 USB JTAG 影响吗」——**问到点子上了，而且原设计确实有问题**。

**机制核实（依据 IDF v6.1 源码）**：

| 事实 | 依据 |
|---|---|
| C6 只有**一个** USB Serial/JTAG 外设（IO12/IO13），PC 上就是一个 COM 口 | 一对 D-/D+，无法分裂成两个设备 |
| 工程级 `sdkconfig.defaults` 开了 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`，会把日志同时写 USB | `esp_stdio/stdio_simple.c` 在 `!ESP_ROM_CONSOLE_OUTPUT_SECONDARY` 时对每个字符调用 `esp_rom_usb_serial_putc()`；`stdio_vfs.c` 还把 USB 注册为 VFS 辅助输出 |
| 结果 | 雷达原始数据与中文日志写入**同一个 FIFO**，上位机收到交错混合流 |
| JTAG 本身**不干扰** | 它是同一外设的分时复用功能，不开调试器时不占链路 |

**处置（三道防线）**：

1. **构建系统层**：新增 `sdkconfig.defaults.radar_bridge`（内含 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`）；
   `build_all.ps1` 用 IDF 的 `SDKCONFIG_DEFAULTS` 多文件机制（分号分隔，**后者覆盖前者**）自动附加该文件，
   **只对 radar_bridge 生效**，不影响其它 demo 原本「插 USB 就能看日志」的便利。
2. **源码层硬保护**：`radar_bridge` 顶部加 `#error`——
   若 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` 或 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` 为真，
   直接编译失败并打印改法（**已实测：故意改回开启 → 构建失败并给出提示**；也可用 `-D EDK60_ALLOW_USB_LOGS` 显式放行）。
   ⚠ **这里踩过一个自己造的坑（已修）**：本工程的 16 个 demo 源文件是**一起编译**的，
   所以第一版写成无条件 `#if defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG) #error` 之后，
   **除 radar_bridge 外的 15 个 demo 全部编译失败**（工程级 defaults 默认就开着该选项）。
   修法：保护前面加 `defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE) &&`，
   让 `#error` **只在构建目标正好是 radar_bridge 时**生效。
   验证：`hello_board` 在 USB 第二控制台开启的情况下正常编过（174.4 KB）；
   `radar_bridge` 在被改回开启时仍然编译失败并给出提示。
   **教训：在「一起编译」的工程里加编译期断言，必须同时约束「当前选中的是哪个 demo」。**
3. **文档层**：README 的「三步走」加了链路分工表；pinmap 的通信接口表新增「USB 是单一外设」条目；
   Kconfig 的 `UNMUTE_LOGS` 选项 help 改写了真实代价说明。

**修正后的验证**：`sdkconfig.radar_bridge` 中确认为
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y` + `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
（USB 第二输出口已关闭）→ USB 上只跑数据。

---

## 七、经验沉淀（供后续项目复用）

1. **子代理契约里必须写「只追加，不重写」**——尤其是多方共享的配置文件（Kconfig/CMake/settings）。否则并发写入必然覆盖。
2. **共享的生成物（sdkconfig/build 目录）必须按代理隔离**，否则会出现难以复现的竞态错误。
3. **集成验证要在隔离副本里做，且必须在所有写者停手之后同步一次**——否则会把过期快照的错误误判给 owner。
4. **硬件结论属于「需要交叉复核」的高风险信息**：本次雷达电源极性就是典型——低分辨率整图判读会读反，必须高分辨率分区渲染 + 逐层追线。
5. **工具链特有的格式宏行为**（`PRIu32` 展开取决于 ABI）应在项目早期写一个最小复现实验确立，而不是等 30 处报错。
6. **Kconfig 的三条硬约束值得写进团队规范**：① `choice` 不产生值符号（要另加派生 `int`/`string`）；② `default n` 的 bool 不生成宏（C 侧要 `#ifdef` 兜底）；③ `sdkconfig.defaults` 不支持行尾注释。
7. **生成的配置文件会"粘住"旧状态**：Kconfig 改动后必须重新生成 sdkconfig，否则新符号缺失会被误判成代码问题。回归脚本应在每次构建前清掉该产物。
8. **「便利型默认值」会变成「数据型场景的坑」**：`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` 对多数 demo 是便利（插 USB 顺手看日志），
   但对「用 USB 送数据」的 demo 就是污染源。教训：**同一份默认配置服务不了所有场景**，
   应当用 per-demo 覆盖（`SDKCONFIG_DEFAULTS` 多文件）把它们分开，并在源码里加编译期保护，
   让人「不可能在不知不觉中」把数据流搞脏。
