# EDK60 桥接 EDV11P（纯透传）技术可行性与实现方案

> 文档编号：EDK60-EDV11P-TRACK / T0-1
> 版本：v1.0（2026-10-21）｜ 作者：嵌入式/软件工程师（opcs-sw-engineer）
> 阶段定位：**P0 方案冻结（Gate-1 材料）**。本文只做分析与改动清单，**不含任何代码/配置改动**（代码在 P1 才落地）。
> 已加载技能：`emb-driver`、`emb-arch`。
> 结论口径：所有结论必须能回溯到「工程内文件:行号」或「ESP-IDF 组件源码/Kconfig:行号」；无法回溯的一律进第 9 章「未确认项」。

---

## 0. 结论速览（TL;DR）

| 问题 | 结论 |
|---|---|
| 可行吗 | **可行，且风险主要在「USB 同一端点上的控制台字符」，不在协议与带宽** |
| 新 demo 还是改旧的 | **新建 `radar_bridge_edv11p`**；不改 4.3 `usb_serial_bridge`、不改 5.1 `radar_bridge`（理由见第 1 章） |
| 控制台洁净首选 | **保持 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` 不动 + `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` + 源码零 `printf`（诊断直写 UART0，默认关闭）**（第 2 章） |
| 是否需要雷达握手 | **完全不需要**：EDV11P 无 `turn_on` 类命令，工作模式默认就是 1（`docs/EDV11P串口通用协议.md:72`），配置全由上位机 GET/SET |
| 带宽够不够 | 够。921600 bps = 92,160 B/s；最坏 512 B/10 ms = 51,200 B/s（占线速 55.6%），USB FS 侧余量 > 10×（第 3 章） |
| 会不会污染二进制流 | 只要做到「零字符输出」，USB 上就**只有数据**；唯一残留是 panic/assert 输出（可接受，见 2.6） |
| 产物 | `build_all.ps1 -Demo radar_bridge_edv11p` + `idf.py -B build-radar_bridge_edv11p merge-bin`，预计 app ≈ 215 KB / merged ≈ 282 KB（第 6 章） |

---

## 1. 方案结论：新建 `radar_bridge_edv11p`

### 1.1 四个候选方案对比

| # | 方案 | 优点 | 致命缺点 | 结论 |
|---|---|---|---|---|
| A | **新建 demo `radar_bridge_edv11p`（5.2）** | 与既有 demo 完全解耦；可为它单独挂 `sdkconfig.defaults.radar_bridge_edv11p`（`build_all.ps1:174-179` 已支持）做「静音 + 921600 无回显 + 大缓冲」；零回归 | 多编一个源文件（构建时间 +约 1 个文件） | **✅ 采纳** |
| B | 改造 4.3 `usb_serial_bridge` | 复用现成双向搬运框架 | ① 它是**教学 demo**：本地回环（`CONFIG_APP_EDK60_BRIDGE_LOCAL_ECHO`，`demo_get_started_usb_serial_bridge.c:129-133`）与「带帧头调试视图」（`:194-227`）是它的观察点，改成纯透传等于废掉这个 demo；② 它的引脚叙事已过期且自相矛盾（见 1.2）；③ 为了静音要在全局 `sdkconfig.defaults` 里降日志级别，会连带改变 4.3 的演示体验 | ❌ 否 |
| C | 改造 5.1 `radar_bridge`（EDV151） | 已上板验证、已有大缓冲与双任务框架 | 它内置 **6 条 EDV151 私有握手**（`demo_get_started_radar_bridge.c:779-810`：`turn_on`/`set_time`/`set_para`/`set_gain 0.5`/`set_boost 2`）。EDV151 的体征出数**完全依赖**这些命令（`docs/EDK60-pitfalls.md` G5）；且该 demo 已被 `docs/tutorial-edv151-vital-signs.md` 与线上 release 固件引用。改它 = 破坏已交付产品 demo | ❌ 否 |
| D | 在 5.1 里加一个编译开关切 EDV11P | 少一个文件 | 一个文件里塞两套协议叙事 + 两套 sdkconfig 语义，`#if` 嵌套会让「无条件 `#error` 误伤全量构建」的历史坑（`docs/EDK60-pitfalls.md` B1）复发概率升高；且 5.1 的注释/日志全部是 EDV151 语义，维护者极易误读 | ❌ 否 |

**采用方案 A。** 同时满足章程范围基线（`docs/project/edv11p/charter.md:29` 明确「新固件 demo `radar_bridge_edv11p`」）。

### 1.2 为什么不能「顺手」改 4.3（实证）

4.3 的引脚叙事在 2026-10-15 实测定论后**没有同步**，现在文件内部三处互相矛盾：

| 位置 | 说法 |
|---|---|
| `demo_get_started_usb_serial_bridge.c:16-21`（文件头） | 模组 TX→IO4、MCU RX←IO5、EN=IO14（正确的实测结论） |
| `demo_get_started_usb_serial_bridge.c:25-35`（文件头） | 仍说「H4 没引出 MCU→模组数据线，要改板才行」 |
| `demo_get_started_usb_serial_bridge.c:420`（代码注释） | 「TX = IO5 已接好……可向模组下发命令」 |
| `demo_get_started_usb_serial_bridge.c:421-423`（实际调用） | `uart_set_pin(..., UART_PIN_NO_CHANGE, EDK60_PIN_RADAR_UART_RX, ...)` —— **TX 根本没绑**，所以「USB→雷达」实际不生效 |

把它改成 EDV11P 正式 demo，等于要同时重写它的全部注释与文档引用（`docs/tutorial-edv151-vital-signs.md`、`docs/flash-guide-for-beginner.md` 都指向 4.3/5.1 的叙事），风险远大于新建文件。

### 1.3 对既有 16 个 demo 的零回归论证

| 回归面 | 依据 | 影响 |
|---|---|---|
| 构建期 | 组件编译**全部**源文件（`components/edk60_app/CMakeLists.txt:22-40`），新增一行只是多编一个文件 | 无 |
| 分派期 | `demo_dispatch.c:27-77` 用 `#if defined(CONFIG_...)` 链式分派，新分支插在 `:72-74` 之后，不动 `#else #error` | 无 |
| 链接期 | `--gc-sections` 丢弃未选中 demo（`CMakeLists.txt:14` 注释 + `docs/EDK60-pitfalls.md` A6 已验证 `nm` 符号缺失） | 无 |
| 配置期 | 新 demo 用**独立** `sdkconfig.defaults.radar_bridge_edv11p`；5.1 的 `sdkconfig.defaults.radar_bridge` 一字不动 | 无 |
| Kconfig | 只在 `choice APP_EDK60_DEMO`（`main/Kconfig.projbuild:12-56`）里**追加**一个成员 + 追加一个新 `menu`，不改任何既有 `config` | 无 |
| 断言 | 所有 `#error` 一律写成 `#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P) && …`（铁律，见 `docs/EDK60-pitfalls.md` B1） | 无 |

---

## 2. 控制台洁净性方案（最高优先风险）

### 2.1 先把机制说清：为什么日志一定会和二进制流撞在一起

IDF v6.1 的控制台是**一个 VFS 复用器**：primary（全部能力）+ auxiliary（只写）两个后端，写操作**扇出**到所有后端。

- `C:\esp\v6.1\esp-idf\components\esp_stdio\stdio_vfs.c:31-34, 427-432`：当 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` 时，执行
  `register_auxiliary(esp_vfs_usb_serial_jtag_get_vfs(), …)` —— 即**所有** stdout/stderr 写（`printf`、`puts`、`ESP_LOG*`）都会同时写给 UART0 **和** USB。
- 本工程正是这个配置：`sdkconfig.defaults:29` `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`。

由此得到三条硬事实：

| # | 事实 | 证据 |
|---|---|---|
| F1 | 主控制台**已经是** UART0（IO16/IO17），不是 USB | `sdkconfig.defaults:20-29`；实测构建产物 `build-radar_bridge/config/sdkconfig.h` 里 `CONFIG_ESP_CONSOLE_UART_DEFAULT 1` + `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG 1`；Kconfig 默认值见 `esp-idf/components/esp_stdio/Kconfig:2-4` |
| F2 | 次级（USB）后端的写是**逐字符直写 USB FIFO**，且遇 `'\n'` 立即 flush | `esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c:148-171`（`usb_serial_jtag_tx_char_no_driver`：`usb_serial_jtag_ll_write_txfifo` + `if (c == '\n') usb_serial_jtag_ll_txfifo_flush()`） |
| F3 | 次级模式**不会**安装 USJ 驱动（`usb_serial_jtag_driver_install` 只在 primary=USJ 时调用），所以「控制台写」与「我们的驱动写」是**两个互不持锁的写者** | `usb_serial_jtag_vfs.c:678-714`（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` 分支才 install）vs `:716-723`（secondary 只注册 VFS） |

**F2 + F3 的组合后果**：一行日志不是「追加在帧后面」，而是**可能在帧中间插入若干字节**，且 `'\n'` 会触发 FIFO flush —— 上位机解析器只能把它当作坏帧丢弃，并滑一个字节重新找 `0x45 0x44`（`human_track_v1.4_side_mount.py:243-276`）。这就是 R1（风险登记册 R=20）的物理机制。

### 2.2 两次既有上板实测（直接引用，不重复推断）

| 实测 | 配置 | 结果 | 出处 |
|---|---|---|---|
| 实测 1 | `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` | 启动**卡死在** `sleep_gpio: Enable automatic switching of GPIO sleep configuration` 之后再无输出，USB 不枚举，必须断电重插 | `sdkconfig.defaults.radar_bridge:8-18`；`docs/EDK60-pitfalls.md` D1 |
| 实测 2 | 仅 `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` | 只压住 `ESP_LOG*`；`printf` 仍经控制台扇出，实测抓到 **1142 字节**文本污染 | `sdkconfig.defaults.radar_bridge:20-23` |

实测 1 的根因在 IDF 源码里可验证：`esp-idf/components/esp_stdio/Kconfig:57-61`

```kconfig
config ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    bool
    default y if ESP_CONSOLE_USB_SERIAL_JTAG || ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    select USJ_ENABLE_USB_SERIAL_JTAG
```

两个控制台都从 USB 撤掉 ⇒ `ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=n` ⇒ 不再 `select USJ_ENABLE_USB_SERIAL_JTAG`；而该符号的 help 明写「disable this option, then **the clock of this module will be disabled at startup**」（`esp-idf/components/esp_driver_usb_serial_jtag/Kconfig:4-15`）。时钟一关，USB 不再枚举。

### 2.3 候选方案排序（含「为什么被否」）

| 排序 | 方案 | 怎么配（精确到符号/API） | 副作用 | 可验证方法 | 判定 |
|---|---|---|---|---|---|
| **① 首选** | **保留 secondary 不动 + 全局静音 + 源码零 printf** | ① `sdkconfig.defaults.radar_bridge_edv11p` 写 `CONFIG_LOG_DEFAULT_LEVEL_NONE=y`；② `# CONFIG_APP_EDK60_STARTUP_BANNER is not set`、`# CONFIG_APP_EDK60_PRINT_PINMAP is not set`（横幅/引脚表是 `printf`，见 `edk60_board.c:57-110`）；③ 新 demo 里**不出现任何** `printf`/`ESP_LOG*`/`puts`；④ 诊断改「直写 UART0」且 Kconfig 默认关闭 | 启动后 USB 上无任何诊断；出问题只能靠 UART0 或 PC 侧抓包 | 见 2.5 的 raw-dump：`未归属字节 = 0` | **✅ 唯一推荐** |
| ② 次选（仅调试期） | 首选 + 开启 `APP_EDK60_EDV11P_DIAG_ON_UART0` | `uart_driver_install(UART_NUM_0, …)` 后立刻 `uart_vfs_dev_use_driver(UART_NUM_0)`，再用 `uart_write_bytes(UART_NUM_0, …)` 发诊断 | 诊断只在 UART0（H2 D1，115200）；控制台改走驱动（`esp-idf/components/esp_driver_uart/include/driver/uart_vfs.h:81-94` 明确：默认是 nonblocking busy-wait，必须 `use_driver` 才不会双写同一 FIFO） | 同时看 UART0 有字、USB raw-dump 仍 0 字节非帧数据 | ✅ 可选 |
| ③ 否 | 把 primary 控制台「移到」UART0 | —— | **这已经是现状**（F1）。以为它还没移，就会去改 `choice ESP_CONSOLE_UART`，改了只可能更糟（选 `ESP_CONSOLE_NONE` 会连 UART0 日志都没了） | `sdkconfig.h` 里 `CONFIG_ESP_CONSOLE_UART_DEFAULT 1` | ❌ 伪需求 |
| ④ 否 | `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` | Kconfig 成员 | 实测启动卡死（2.2 实测 1） | 上板即复现 | ❌ **严禁** |
| ⑤ 否 | 只 `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` | Kconfig 成员 | `printf` 仍扇出（实测 1142 B）；且 `/dev/console` 任何写入都会污染 | raw-dump 数出文本字节 | ❌ 不充分 |
| ⑥ 否 | 保留日志，但「保证输出不含 0x45 0x44」 | 需重写全部日志文案 + 逐字符过滤 | ① 日志本身是 `printf` 实现（`edk60_board.c:57-110`），要过滤就得接管 `vprintf`（`esp_log_set_vprintf`），但 `printf` 不走它 → 仍会漏；② 更致命的是 F2：日志插入帧中间时，**即使不含 0x45 0x44 也会破坏该帧的长度/校验**，让一整帧作废，不只是「多一个假帧」 | 构造压力测试：帧率 10 Hz 下每 100 ms 打一行日志，统计坏帧 | ❌ 物理上不成立 |
| ⑦ 否 | `freopen("/dev/null", "w", stdout)` | libc 调用 | 它只替换 stdout 的 `FILE*`；而 `ESP_LOG*` 走 `esp_log_write()` → 内部 vprintf → 直接写 VFS 控制台设备（`/dev/console` 的 primary + auxiliary 双写，`stdio_vfs.c:427-432`），**不经过 stdout 的 FILE\***，因此日志照旧上 USB；且 `edk60_board.c:57-110` 的 `printf` 与 `ESP_LOGI` 混用，改一处不影响另一处 | 打开上位机，仍能在二进制流里看到 `esp_stdio`/组件日志 | ❌ 不彻底 |

**首选方案 = ①**。它同时满足三个硬约束：不碰 secondary（规避启动卡死）、不依赖「过滤/无害化」（规避 ⑥ 的物理不可能）、失败可观测（用 2.5 的 raw-dump 直接量化）。

### 2.4 首选方案要动到的每一处（可直接照做）

| 序号 | 动作 | 位置 | 说明 |
|---|---|---|---|
| C1 | `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` | 新文件 `sdkconfig.defaults.radar_bridge_edv11p` | `LOG_DEFAULT_LEVEL` 是 **choice**，写成员 `..._NONE=y` 才生效；派生的 `CONFIG_LOG_DEFAULT_LEVEL=0` 由 Kconfig 自动算 |
| C2 | `# CONFIG_APP_EDK60_STARTUP_BANNER is not set` | 同上 | 横幅是 `printf`（`edk60_board.c:57-87`），由 `demo_dispatch.c:86-92` 调用 |
| C3 | `# CONFIG_APP_EDK60_PRINT_PINMAP is not set` | 同上 | 引脚表同样是 `printf`（`edk60_board.c:90-111`） |
| C4 | 新 demo 源文件内**零** `printf` / `ESP_LOGx` / `puts` / `stdout` 写 | `demo_get_started_radar_bridge_edv11p.c` | 连 `ESP_LOGI` 也不行；错误路径只能翻 LED 或（可选）走 C5 |
| C5 | 诊断走 UART0 直写，Kconfig 默认 `n` | `APP_EDK60_EDV11P_DIAG_ON_UART0`（默认 n） | 实现见 5.1 的 `edv11p_diag_init/_diag/_task_diag`；关闭时这三个函数整体不编译 |
| C6 | `demo_dispatch.c` 的两处 `#if CONFIG_…` 改 `#ifdef` 兜底 | `demo_dispatch.c:86, 89, 94` | **必须**：`# CONFIG_APP_EDK60_STARTUP_BANNER is not set` 后该宏**根本不存在**（已验证：`build-radar_bridge/config/sdkconfig.h` 只对选中的符号生成 `#define … 1`），与 `default n` 的 bool 同理（`docs/EDK60-pitfalls.md` A2）。`#if` 遇未定义标识符虽按 0 处理，但本工程把 warning 当门禁，改用 `#ifdef` 一劳永逸 |

### 2.5 「洁净」的可验证方法（不靠感觉）

在 T1-4 的帧校验脚本里内置 **raw-dump + 未归属字节统计**（第 7 章方法 A）：

```
未归属字节率 = (总收字节 − Σ 校验通过帧的字节) / 总收字节
```

| 判定 | 阈值 | 说明 |
|---|---|---|
| 洁净 | 启动后 10 s 起，未归属字节 = 0，且 30 min 内 `0x45 0x44` 假同步 = 0 | 首选方案的目标值 |
| 退化 | 0 < 未归属字节率 < 0.1% | 说明还有零星写者（多半是 panic 或某个模块的 `ESP_EARLY_LOG`），需定位 |
| 失败 | ≥ 0.1%，或出现中文/ASCII 连续可打印串 | 回到 C1~C4 逐条自查 |

### 2.6 已知残留污染源（诚实说明，不夸大洁净度）

| 残留源 | 触发条件 | 为什么可以接受 | 处置 |
|---|---|---|---|
| panic / assert 输出 | 崩溃时 | 崩溃本来就是致命事件，此时「USB 上有可读文本」反而是好事；帧流已经不可用 | 不处理；文档写明 |
| `esp_system_console_put_char` 路径 | 任一模块走 ROM 级 `putchar` | C6 无 `ESP_ROM_CONSOLE_OUTPUT_SECONDARY`，该函数在 `esp-idf/components/esp_stdio/stdio_simple.c:12-17` 会额外调用 `esp_rom_usb_serial_putc` → 直写 USB | 仅在早期启动/panic 发生；正常运行时无调用者 |
| 启动阶段 ROM/bootloader 日志 | 复位后 ~100 ms | bootloader 走 UART0（primary），不走 `esp_stdio` 复用器；C6 无 ROM secondary | 抓包若在开头看到文本，只统计 `T+10 s` 之后 |

---

## 3. 透传通路设计（`emb-arch` / `emb-driver`）

### 3.1 总体数据流

```
EDV11P 模组                EDK60（ESP32-C6）                        PC
┌──────────┐  UART1 921600 8N1  ┌───────────────────────────┐  原生 USB FS  ┌──────────────┐
│ TX (H3-T)├───────────────────►│ IO4 → uart_read_bytes     │              │              │
│          │                    │        ↓ task_r2u(prio 6)  │              │ human_track  │
│ RX (H3-A)│◄───────────────────┤ IO5 ← uart_write_bytes    │  USJ 端点    │ _v1.4_side_  │
│ EN: IO14 │   （仅转发上位机帧） │        ↑ task_u2r(prio 6)  │◄────────────►│ mount.py     │
└──────────┘                    │ usb_serial_jtag_read/write│              │ (PySide6)    │
                                └───────────────────────────┘              └──────────────┘
```

要点：**板子不解析、不生成、不修改任何 EDV11P 帧**；不注入握手；不做文本日志。

### 3.2 UART1 侧配置（逐项）

| 项 | 值 | 依据 |
|---|---|---|
| 端口 | `UART_NUM_1` | UART0 被控制台占用（`sdkconfig.defaults` 主控制台 = UART0）；C6 的 UART1/2 无 IOMUX 默认脚，必须显式绑（`main/Kconfig.projbuild:484-486` 同理） |
| 波特率 | 921600 | `docs/EDV11P串口通用协议.md:20`「波特率：921600(默认)」 |
| 帧格式 | 8N1、无流控 | 同上 `:22-28`（数据位 8 / 无校验 / 停止位 1 / 无流控） |
| RX | `EDK60_PIN_RADAR_UART_RX` = **IO4** | `components/edk60_board/include/edk60_board.h:147`（2026-10-15 实测：H3 的 T 脚） |
| TX | `EDK60_PIN_RADAR_UART_TX` = **IO5** | 同上 `:148`（实测 MCU→模组 RX，H3 的 A 脚） |
| EN | `EDK60_PIN_RADAR_PWR_EN` = **IO14**，常高 | 同上 `:149-150`；`edk60_radar_power_init()/on()`（`edk60_board.h:312-321`） |
| RX 环形缓冲 | 8192 B | 921600 → 92,160 B/s ⇒ **88.9 ms** 的调度余量（5.1 用同量级，见 `demo_get_started_radar_bridge.c:195`） |
| TX 环形缓冲 | 2048 B | 上位机的 Set/Get 帧都是 11~40 B，2048 B = 数十帧，足够 |
| `uart_set_rx_timeout` | 2~5（符号时间） | 触发 FIFO 超时中断，让短帧尽快交付；5.1 用的是 5（`:329`） |
| `uart_flush_input` | **仅初始化时一次**（建任务之前） | 运行期 flush 会与读任务争 UART 接收锁而死锁（`docs/EDK60-pitfalls.md` E1） |
| 命令注入 | **无** | 本 demo 不调用 `uart_write_bytes` 发送任何常量帧；TX 只转发上位机字节 |

> **为什么 TX 必须接上**：`human_track_v1.4_side_mount.py` 连上 COM 后 100 ms 就发 `GET 0x02`，随后在 ~750 ms 内连发 15 条 GET（`human_track_v1.4_side_mount.py:1867, 2986-3044`）。没有 TX，界面永远是空白 —— 这正是 4.3 demo 现状的坑（第 1.2 节）。

### 3.3 USB 侧配置

| 项 | 值 | 依据/理由 |
|---|---|---|
| 驱动 | `usb_serial_jtag_driver_install()`，先 `usb_serial_jtag_is_driver_installed()` 复用 | 幂等；5.1 同做法（`demo_get_started_radar_bridge.c:369-389`）。本工程 BSP 的 `edk60_console_install_usb_serial_jtag()`（`edk60_board.c:508-525`）在「primary=UART0」的配置下走 `#else` 分支返回 `ESP_ERR_NOT_SUPPORTED` 且**当前无任何调用者**，所以次级控制台模式下去驱动是**由本 demo 独占安装**的 |
| TX 环形缓冲 | **8192**（默认只有 256） | `USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT()` 的 `tx_buffer_size`/`rx_buffer_size` 默认都是 256（`esp-idf/components/esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h`）。注意：**次级控制台模式不会安装驱动**（F3），所以我们装的就是唯一驱动，缓冲区我们说了算 |
| RX 环形缓冲 | 2048 | 上位机 Set 帧突发量极小（<1 KB） |
| 写锁 | 1 把 mutex | 若未来有人加第二个写者，避免帧被撕裂；当前只有 `task_r2u` 一个写者 |
| 写超时 | 200 ms/次，超时后**丢弃剩余并计数** | 见 3.5 背压点② |

### 3.4 任务设计（`emb-arch` 输出格式）

| 任务 | 优先级 | 触发 | 栈估计 | 通信 | 说明 |
|---|---|---|---|---|---|
| `edv11p_r2u`（雷达→USB） | 6 | `uart_read_bytes(…, pdMS_TO_TICKS(20))` 阻塞读 | 4096 B（含 512 B 栈内 buf） | 无（单向流） | 每次最多搬 `CHUNK=512` B（≈ 单帧上限，见 3.6），整包写 USB |
| `edv11p_u2r`（USB→雷达） | 6 | `usb_serial_jtag_read_bytes(…, pdMS_TO_TICKS(20))` | 4096 B | 无 | 每次最多 256 B，`uart_write_bytes` 入队；失败只计数 |
| `edv11p_diag`（可选，仅 DIAG=y） | 2 | 周期 `STATS_SEC` | 3072 B | 读共享计数器 | 直写 UART0，**不进** VFS 控制台 |
| 统计计数器 | — | 两任务自增 | 静态 | `portMUX` 临界区 | 5.1 用同一手法（`demo_get_started_radar_bridge.c:226-239`） |

**中断**：本设计不注册任何自定义 ISR —— UART 与 USJ 都由 IDF 驱动内部的 ISR 把数据搬进环形缓冲；应用层全在任务上下文。这是刻意的：`emb-driver` 要求「中断里只做最轻的事」，而这里的「最轻的事」IDF 已经做完了。

**任务数与优先级选择的理由**（`emb-arch` 决策点 1~3）：
- 用 RTOS 而非裸机：需要两个方向各自独立阻塞（否则任一方向卡住会拖死另一方向），这是 RTOS 阻塞 I/O 的标准场景。
- 两个搬运任务**同优先级**即可：两个方向物理独立（UART 全双工 + USB 双向独立端点的环形缓冲），互相之间没有共享资源（唯一共享是日志计数器，临界区保护）。
- **tick 频率建议 1000 Hz**：默认 `CONFIG_FREERTOS_HZ=100`（`esp-idf/components/freertos/Kconfig` 中 `config FREERTOS_HZ … default 100`）⇒ 10 ms 粒度，`pdMS_TO_TICKS(20)` 的抖动可达 ±10 ms。在本 demo 的 `sdkconfig.defaults.radar_bridge_edv11p` 里设 `CONFIG_FREERTOS_HZ=1000`（只影响本 demo 的 sdkconfig，不改其它 demo）可把读取超时抖动压到 ±1 ms。

### 3.5 背压与丢包风险点（逐点量化）

| # | 风险点 | 容量 | 折算可容忍时长 | 溢出表现 | 缓解 |
|---|---|---|---|---|---|
| ① | UART RX 环形缓冲 | 8192 B | **88.9 ms** @92,160 B/s | `uart_read_bytes` 拿到的字节缺失（**静默丢字节**，因为 UART 无重传） | 读任务优先级 6 + 超时 20 ms；`CHUNK=512` 让单次搬运覆盖整帧；tick 提到 1000 Hz |
| ② | USB TX 环形缓冲 | 8192 B | **160 ms** @51,200 B/s（最坏帧率） | `usb_serial_jtag_write_bytes` 短写/超时 | 循环写；200 ms 超时后丢弃剩余并计数；上位机没打开时不刷屏 |
| ③ | USB RX 环形缓冲 | 2048 B | 相当于 ~50 帧 Set | 上位机帧丢失（上位机会重试/用户重发） | 上位机突发只有 15 帧（`:2986-3044`），余量 20× |
| ④ | UART TX 环形缓冲 | 2048 B | 22 ms @921600 | `uart_write_bytes` 阻塞，把 `u2r` 任务挂住 | 独立任务，不阻塞 `r2u`；雷达正常时会 ACK，很快腾空 |
| ⑤ | PC 侧读得慢（PySide 事件循环被绘图/视频录制卡住） | — | — | ①→②连锁 | 这是**唯一真正的丢包路径**，必须靠第 7 章 30 min 压测量化；必要时改 USB TX 为 16384 B |

吞吐估算表（用协议上限口径）：

| 场景 | 帧长 | 周期 | 稳态码率 | 占 UART 线速 | 占 USB FS（理论 1.216 MB/s） |
|---|---|---|---|---|---|
| 典型：0x70，1 感应区 4 目标（`协议.md:220-235` 的样例） | 值 30 B → 整帧 **41 B** | 100 ms（`0x10=2`，单位 50 ms，见 9-U1） | 0.41 kB/s | 0.44% | 0.03% |
| 较满：0x70，1 感应区 10 目标 | 值 72 B → 整帧 83 B | 100 ms | 0.83 kB/s | 0.9% | 0.07% |
| 极端假设：整帧取 512 B 上限（`edv11p.h:32` `EDV11P_PRO_MAX_FRAME_LEN 512`） | 512 B | 10 ms | **51.2 kB/s** | **55.6%** | 4.2% |
| 极端假设：512 B | 512 B | 50 ms | 10.2 kB/s | 11.1% | 0.8% |

结论：**带宽不是风险，串行缓冲与调度抖动才是**。所以设计上把 RX 缓冲做到 8 KB（≈89 ms）、USB TX 做到 8 KB（≈160 ms）。

### 3.6 上位机 Set 帧下发 与 雷达上报 的并发是否安全

**安全。** 理由分三层：

1. **物理层**：UART 是**全双工**（TX/RX 独立引脚与独立 FIFO），C6 的 UART1 收发互不影响；USJ 的 IN/OUT 端点也是独立的。
2. **驱动层**：两个搬运任务访问的是**不同**的驱动与**不同**方向：`r2u` 只 `uart_read_bytes` + `usb_serial_jtag_write_bytes`；`u2r` 只 `usb_serial_jtag_read_bytes` + `uart_write_bytes`。没有共享缓冲，只有共享计数器（临界区）。
3. **协议层**：EDV11P 的 Get/Set 与状态上报本身就是异步的（`协议.md:107`「Get/Set 命令下发采用异步模式，接收方使用 Report 进行回复」；`:120` 主动上报用 Status）。模组侧的实现细节不在我们控制范围内，但**桥接器不改变任何时序**（零延迟转发，不缓存、不重排）—— 这正是「纯桥接」在协议一致性上的优势：任何握手/时序问题都能在「雷达直连 USB-TTL」的 A/B 对照里重现（第 7 章方法 B）。

**唯一需要注意的并发细节**：不要给 USB 写加「整帧锁」式的假设。上位机已知 `0x45 0x44` 前可能是任意字节并会重同步（`human_track_v1.4_side_mount.py:243-252`），所以即使发生「一帧被拆成两次 USB 写」，也不会被误判。

---

## 4. Kconfig 改动清单

### 4.1 三条硬规则的合规写法（本项目已多次踩）

| 规则 | 后果 | 本方案的合规做法 |
|---|---|---|
| R-K1 `choice` 不产生值符号 | 想用 `CONFIG_APP_X_UART_PORT` 取端口号会 `undeclared`（`docs/EDK60-pitfalls.md` A1；`main/Kconfig.projbuild:494-498` 就是为此加的派生 int） | 新菜单**不使用任何 choice**；UART 端口在源码里固定 `UART_NUM_1` 并加 `_Static_assert`/注释说明「UART0 归控制台」。若未来要可选，必须仿 `:495-498` 补派生 `int` |
| R-K2 `default n` 的 bool 不生成宏 | 直接 `#if CONFIG_X` / 在表达式里引用会 `undeclared`（`pitfalls` A2，已踩 3 次） | 新配置里唯一的 `default n` 项是 `APP_EDK60_EDV11P_DIAG_ON_UART0`，源码里**一律** `#ifdef` 兜底转 0/1（写法模板见 5.1 末尾的编译期断言与 `BRIDGE_LOCAL_ECHO` 式映射） |
| R-K3 `sdkconfig.defaults` 不支持行尾注释 | 整行被**静默忽略**（`pitfalls` A3；`sdkconfig.defaults:6-11` 有记录） | 4.4 给出的文件内容里，注释**全部独占一行**，且每行都是 `KEY=VALUE` 或 `# KEY is not set` |

### 4.2 `main/Kconfig.projbuild` —— demo choice 追加（`main/Kconfig.projbuild:54-55` 之后）

```kconfig
        config APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P
            bool "5.2 radar_bridge_edv11p EDV11P 人数统计/轨迹 ⇄ UART ⇄ USB（纯透传，不注入命令）"
```

- 所属 menu：`EDK60 Demo`（`main/Kconfig.projbuild:10`）
- 所属 choice：`APP_EDK60_DEMO`（`:12-56`）
- 类型：`bool`（choice 成员，无需 default —— choice 的 `default APP_EDK60_DEMO_HELLO_BOARD` 在 `:14`，保持不变）

### 4.3 新增 menu（文件末尾 `:645` 之后追加）

| 符号 | 类型 | 默认值 | 范围 | 说明 |
|---|---|---|---|---|
| `APP_EDK60_EDV11P_RX_PIN` | int | `4` | 0~23 | 模组 TX→MCU（H3 的 T = IO4） |
| `APP_EDK60_EDV11P_TX_PIN` | int | `5` | -1~23 | MCU→模组 RX（H3 的 A = IO5）；-1 = 不接（只读） |
| `APP_EDK60_EDV11P_BAUD` | int | `921600` | 9600~3000000 | `协议.md:20` |
| `APP_EDK60_EDV11P_PWR_EN_ON_BOOT` | bool | `y` | — | 启动即拉高 IO14 |
| `APP_EDK60_EDV11P_PWR_READY_MS` | int | `800` | 0~5000 | 上电到模组就绪的等待（**非实测值**，见 9-U5） |
| `APP_EDK60_EDV11P_UART_RX_BUF` | int | `8192` | 1024~65536 | 88.9 ms 余量 |
| `APP_EDK60_EDV11P_USB_TX_BUF` | int | `8192` | 512~65536 | 默认 256 不够 |
| `APP_EDK60_EDV11P_USB_RX_BUF` | int | `2048` | 256~65536 | 上位机 Set 突发量小 |
| `APP_EDK60_EDV11P_CHUNK` | int | `512` | 64~2048 | = 单帧上限（`edv11p.h:32`） |
| `APP_EDK60_EDV11P_RD_TIMEOUT_MS` | int | `20` | 1~200 | 与 1000 Hz tick 配合 |
| `APP_EDK60_EDV11P_USB_WRITE_TIMEOUT_MS` | int | `200` | 10~2000 | 背压点②的丢弃阈值 |
| `APP_EDK60_EDV11P_DIAG_ON_UART0` | bool | **`n`** | — | 诊断直写 UART0；`#ifdef` 兜底 |
| `APP_EDK60_EDV11P_STATS_SEC` | int | `30` | 0~600 | 仅 DIAG=y 时生效 |

menu 标题：`menu "EDK60 Demo 参数（EDV11P 透传桥 5.2）"`（与既有 `EDK60 Demo 参数（雷达透传桥 5.1）` 并列，`:473`）。

### 4.4 新增 `sdkconfig.defaults.radar_bridge_edv11p`（逐行，合规写法）

```text
# ===========================================================================
# radar_bridge_edv11p（demo 5.2）专用默认配置
# 覆盖关系：sdkconfig.defaults  <-  sdkconfig.defaults.esp32c6  <-  本文件
# （build_all.ps1:174-179 用 SDKCONFIG_DEFAULTS 分号列表，后者覆盖前者）
# 语法限制：本文件只支持 KEY=VALUE，注释必须独占一行（不允许行尾注释）
# ===========================================================================

# ---- 控制台洁净性（首选方案，见 analysis-bridge-feasibility.md 第 2 章）----
# 1) 压掉所有 ESP_LOG*（实测：只压这个不够，printf 仍会扇出到 USB）
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
# 2) 横幅与引脚表是 printf 实现，必须关掉（它们由 demo_dispatch.c 调用）
# CONFIG_APP_EDK60_STARTUP_BANNER is not set
# CONFIG_APP_EDK60_PRINT_PINMAP is not set
# 3) 绝对不能碰次级控制台：CONFIG_ESP_CONSOLE_SECONDARY_NONE 实测会导致启动卡死
#    （证据：sdkconfig.defaults.radar_bridge:8-18 / EDK60-pitfalls.md D1）
# 4) 诊断默认关闭；排查时改成 =y，诊断只走 UART0，不碰 USB
# CONFIG_APP_EDK60_EDV11P_DIAG_ON_UART0 is not set

# ---- 时序 ----
# tick 从 100 Hz 提到 1000 Hz，降低读取超时的调度抖动（只影响本 demo 的 sdkconfig）
CONFIG_FREERTOS_HZ=1000

# ---- 引脚与串口（2026-10-15 实测定论，见 edk60_board.h:130-154）----
CONFIG_APP_EDK60_EDV11P_RX_PIN=4
CONFIG_APP_EDK60_EDV11P_TX_PIN=5
CONFIG_APP_EDK60_EDV11P_BAUD=921600
CONFIG_APP_EDK60_EDV11P_PWR_EN_ON_BOOT=y
CONFIG_APP_EDK60_EDV11P_PWR_READY_MS=800

# ---- 缓冲 ----
CONFIG_APP_EDK60_EDV11P_UART_RX_BUF=8192
CONFIG_APP_EDK60_EDV11P_USB_TX_BUF=8192
CONFIG_APP_EDK60_EDV11P_USB_RX_BUF=2048
CONFIG_APP_EDK60_EDV11P_CHUNK=512
CONFIG_APP_EDK60_EDV11P_RD_TIMEOUT_MS=20
CONFIG_APP_EDK60_EDV11P_USB_WRITE_TIMEOUT_MS=200
```

> demo 选择符号 `CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P=y` **不要**写在本文件里 —— 它由 `build_all.ps1` 写入 `sdkconfig.defaults.esp32c6`（`build_all.ps1:113-124`）。本文件只放本 demo 的差异项。

### 4.5 `build_all.ps1` 登记（必须，否则脚本直接抛错）

在 `$DemoMap`（`build_all.ps1:40-57`）中追加一行：

```powershell
    'radar_bridge_edv11p' = 'APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P'
```

原因：脚本对 `-Demo` 参数做白名单校验，未登记会 `throw "未知 demo: …"`（`build_all.ps1:70-74`）；同时 `-List`（`:59-67`）靠这张表枚举。

---

## 5. 源码改动清单

> 全部为 P1 实施内容，本阶段**不动手**。行号基于当前工作副本。

### 5.1 新增文件：`components/edk60_app/src/demo_get_started_radar_bridge_edv11p.c`

| 函数 | 职责 | 关键约束 |
|---|---|---|
| `void demo_radar_bridge_edv11p_start(void)` | 唯一对外入口：①可选诊断初始化 → ②雷达电源 EN 常高 + 等 `PWR_READY_MS` → ③装/复用 USJ 驱动 → ④装 UART1 并 `uart_flush_input` 一次 → ⑤建两个搬运任务 → ⑥返回 | **不阻塞**（`demo.h:9` 约定）；**全程零字符输出** |
| `static esp_err_t edv11p_usb_install(void)` | `usb_serial_jtag_is_driver_installed()` 复用，否则用显式 `tx_buffer_size=USB_TX_BUF, rx_buffer_size=USB_RX_BUF` 安装；`ESP_ERR_INVALID_STATE` 视为「已安装」 | 与 `demo_get_started_radar_bridge.c:359-389` 同构，但**不**打日志，只返回错误码 |
| `static void edv11p_radar_power_on(void)` | `edk60_radar_power_init()` + `edk60_radar_power_on()` + `vTaskDelay(PWR_READY_MS)`；受 `PWR_EN_ON_BOOT` 控制 | EN **常高、不释放**（`pitfalls` G3 教训：释放 EN = 模组掉电） |
| `static esp_err_t edv11p_uart_install(void)` | `uart_driver_install(UART_NUM_1, UART_RX_BUF, 2048, 0, NULL, 0)` + `uart_param_config`（921600/8N1/无流控）+ `uart_set_pin(UART1, TX_PIN, RX_PIN, NC, NC)` + `uart_set_rx_timeout(…, 5)` + 一次 `uart_flush_input` | TX_PIN<0 时传 `UART_PIN_NO_CHANGE`（纯接收）；**运行期不再 flush**（`pitfalls` E1） |
| `static bool edv11p_usb_write_all(const uint8_t *p, size_t len)` | 循环 `usb_serial_jtag_write_bytes` 直到写完或 `USB_WRITE_TIMEOUT_MS`；持 `s_usb_lock`；短写/超时 → 累计 `s_stat.usb_tx_dropped` | 返回是否整包成功；**不打印任何东西** |
| `static void edv11p_task_r2u(void *arg)` | `uart_read_bytes(UART1, buf, CHUNK, RD_TIMEOUT)` → 若 `>0` 则 `edv11p_usb_write_all` | 超时/负值只计数继续；无 `ESP_LOGW` |
| `static void edv11p_task_u2r(void *arg)` | `usb_serial_jtag_read_bytes(buf, 256, RD_TIMEOUT)` → `TX_PIN>=0` 时 `uart_write_bytes(UART1, …)`，否则丢弃并计数 | **无本地回环**（回环会把上位机自己的帧回灌，导致解析器误判） |
| `static void edv11p_count(volatile uint32_t *f, uint32_t n)` | `portENTER_CRITICAL` 自增 | 与 `demo_get_started_radar_bridge.c:226-239` 同构 |
| `#ifdef CONFIG_APP_EDK60_EDV11P_DIAG_ON_UART0` `static void edv11p_diag_init(void)` / `static void edv11p_diag(const char *fmt, ...)` / `static void edv11p_task_diag(void *arg)` | 诊断：`uart_driver_install(UART_NUM_0, 0, 4096, 0, NULL, 0)` → **立刻** `uart_vfs_dev_use_driver(UART_NUM_0)`（否则控制台与驱动双写同一 FIFO，`uart_vfs.h:81-94`）→ `vsnprintf` 到栈上 buf → `uart_write_bytes(UART_NUM_0, …)` | 仅 UART0；**绝不** `printf`（`printf` 会经 `stdio_vfs.c:427` 扇出到 USB） |

**编译期断言（必须绑定本 demo）**：

```c
/* 铁律：必须 && defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P)，否则 17 个 demo 一起编时全崩
 * （docs/EDK60-pitfalls.md B1） */
#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P) && \
    defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE)
#error "radar_bridge_edv11p: 禁止关闭 USB 次级控制台，会导致 C6 启动卡死（sdkconfig.defaults.radar_bridge:8-18）"
#endif

#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P) && \
    (CONFIG_APP_EDK60_EDV11P_RX_PIN == CONFIG_APP_EDK60_EDV11P_TX_PIN)
#error "radar_bridge_edv11p: RX 与 TX 不能是同一根引脚"
#endif

#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P) && \
    (CONFIG_APP_EDK60_EDV11P_RX_PIN == 14 || CONFIG_APP_EDK60_EDV11P_TX_PIN == 14)
#error "radar_bridge_edv11p: IO14 是雷达电源使能 EN（实测），不能当 UART 用"
#endif
```

> 说明：`CONFIG_APP_EDK60_EDV11P_RX_PIN` 是 int，永远生成宏，可直接在 `#if` 里比较；`DIAG_ON_UART0` 是 `default n` 的 bool，**只能** `#ifdef`（R-K2）。

### 5.2 修改文件清单

| # | 文件 | 改动点 | 具体内容 |
|---|---|---|---|
| M1 | `components/edk60_app/include/edk60_demo.h` | 在 `:44`（5.1 之后）追加声明 | `void demo_radar_bridge_edv11p_start(void);  /* 5.2 EDV11P ⇄ UART1 ⇄ USB 纯字节透传 */` |
| M2 | `components/edk60_app/src/demo_dispatch.c` | 在 `:72-74` 的 `RADAR_BRIDGE` 分支之后、`:75 #else` 之前插入 `#elif` | `#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P)` / `#define EDK60_DEMO_NAME "radar_bridge_edv11p"` / `#define EDK60_DEMO_CALL() demo_radar_bridge_edv11p_start()` |
| M3 | `components/edk60_app/src/demo_dispatch.c` | `:86`、`:89`、`:94` 三处 `#if CONFIG_…` | 改为 `#ifdef` 兜底形式（`APP_EDK60_STARTUP_BANNER` / `APP_EDK60_PRINT_PINMAP` 被 `is not set` 后宏不存在；`BOOT_DELAY_MS` 是 int 可保留 `#if`） |
| M4 | `components/edk60_app/CMakeLists.txt` | `EDK60_DEMO_SRCS`（`:23-40`）追加一行 | `"src/demo_get_started_radar_bridge_edv11p.c"`（`list(LENGTH …)` 会自动变成 17，`:43-44` 的提示随之更新） |
| M5 | `main/Kconfig.projbuild` | demo choice 追加成员 + 文件末尾新增 menu | 见 4.2 / 4.3 |
| M6 | `build_all.ps1` | `$DemoMap`（`:40-57`）追加一条 | 见 4.5 |
| M7 | `sdkconfig.defaults.radar_bridge_edv11p`（新文件） | 见 4.4 | — |

**不需要改**：`main/CMakeLists.txt`（`main` 只是 `REQUIRES edk60_app edk60_board` + 调 `edk60_run_selected_demo()`，`main/src/app_main.c:17`）、`components/edk60_board/`（引脚宏已实测定论）、`sdkconfig.defaults`（工程级默认不动）、`sdkconfig.defaults.radar_bridge`（5.1 专用，一字不动）。

### 5.3 为什么「不注入命令」在代码上也要有可见约束

| 约束 | 实现方式 | 目的 |
|---|---|---|
| 不在启动时发任何帧 | 源码里**不存在**任何 `const uint8_t cmd_…[]` 常量 | 与 5.1 形成对照（`demo_get_started_radar_bridge.c:779-810` 有 6 条命令），评审一眼可辨 |
| 不在运行期发任何帧 | `uart_write_bytes` 只出现在 `task_u2r` 内，且 buffer 来自 `usb_serial_jtag_read_bytes` | 保证「板上唯一字节来源是 PC」 |
| 不做语义解析 | 无 `0x45`/`0x44` 常量、无校验函数、无 msgid 追踪 | 透传层与协议层彻底解耦；协议问题一律在 PC 侧定位 |

---

## 6. 构建与产物

### 6.1 `build_all.ps1 -Demo radar_bridge_edv11p` 可用性

| 检查项 | 结论 | 依据 |
|---|---|---|
| 参数白名单 | 登记后可用，未登记会 `throw` | `build_all.ps1:70-74` |
| demo 选择注入 | 自动写 `sdkconfig.defaults.esp32c6` 的 `CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P=y` | `:113-124, 154` |
| 每次重建 sdkconfig | 会先删 `sdkconfig.<demo>`，避免「新符号不生成」 | `:156-162`；`pitfalls` A4 |
| demo 专用 defaults 自动加载 | `$extraDefaults = sdkconfig.defaults.$Name`，存在则以分号拼到 `SDKCONFIG_DEFAULTS` | `:174-179` |
| 目标芯片 | `-D IDF_TARGET=esp32c6` | `:181-182` |
| 构建目录 | `build-radar_bridge_edv11p/` | `:139` |

命令：

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos
.\build_all.ps1 -Demo radar_bridge_edv11p
```

### 6.2 合并固件

```powershell
idf.py -B build-radar_bridge_edv11p -D "IDF_TARGET=esp32c6" `
       -D "SDKCONFIG=sdkconfig.radar_bridge_edv11p" `
       -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.radar_bridge_edv11p" `
       merge-bin
```

- 产物路径：`build-radar_bridge_edv11p\merged-binary.bin`（实证：同结构产物 `build-radar_bridge\merged-binary.bin` 存在，281,920 B）
- 烧录地址：**0x0**（`bootloader+分区表+app` 全套合并）

### 6.3 体积预估（基于本项目实测样本）

| 样本 | app bin | merged-bin |
|---|---|---|
| `build-radar_bridge`（5.1，含 BLE 栈配置） | 216,384 B（211.3 KB） | 281,920 B（275.3 KB） |
| `build-usb_serial_bridge`（4.3） | 215,184 B（210.1 KB） | — |
| `build-hello_board`（最小） | 178,704 B（174.5 KB） | — |
| **`radar_bridge_edv11p` 预估** | **≈ 210~220 KB** | **≈ 276~286 KB** |

理由：新 demo 的代码量比 5.1 **更小**（无 6 条握手常量、无自动识别扫描逻辑 ~250 行、无嗅探评分），且关掉了横幅/引脚表字符串；`--gc-sections` 会丢掉其余 16 个 demo。分区表 `CONFIG_PARTITION_TABLE_SINGLE_APP=y`（`sdkconfig.defaults:40`），4 MB Flash 余量充裕。

---

## 7. 零丢包 / 一致性验证方法（可执行）

> 落地物：`tools/edv11p_frame_check.py`（T1-4 交付，测试工程师）。本节给出判定口径与脚本骨架。

### 7.1 方法 A：PC 侧逐帧校验（主判据）

**做什么**：

1. 打开串口（原生 USJ 的 CDC 不依赖波特率，但脚本仍按 921600 打开，与上位机一致）。
2. **打开后立刻 `dtr=False, rts=False`**，避免任何自动复位动作（`pitfalls` D3 的教训在裸 UART 桥上也适用；本方案同样保守处理）。
3. 把收到的**原始字节**同时写一份 `raw-dump-<ts>.bin`（这是「洁净性」的唯一证据，见 2.5）。
4. 在线解析：`0x45 0x44` 同步 → 读 `length`（大端）→ 校验 `11 ≤ len ≤ 512`（`MIN=11` 见 `edv11p.h:33`，`MAX=512` 见 `:32`）→ 求和取模 256（`协议.md:52`）→ 通过则计数并统计 `op/type/msg_id`。
5. 统计四类量：**帧数**、**每 type 帧数**、**坏帧/假同步次数**、**未归属字节数**；另统计 `0x70` 的 `msg_id` 连续性与**最大帧间隔**。

**骨架（关键逻辑，非完整实现）**：

```python
import serial, time, collections, sys
HDR, MIN_LEN, MAX_LEN = b'\x45\x44', 11, 512

class Checker:
    def __init__(self):
        self.buf = bytearray()
        self.frames = collections.Counter()   # (op,type) -> count
        self.frame_bytes = 0
        self.total_bytes = 0
        self.bad = 0                          # 长度非法 / 校验失败 / 假同步
        self.msgid_gaps = 0
        self.last_msgid = {}
        self.max_gap = 0.0
        self.last_frame_ts = None

    def feed(self, data, now):
        self.total_bytes += len(data)
        self.buf += data
        while True:
            i = self.buf.find(HDR)
            if i < 0:
                self.buf = self.buf[-1:] if self.buf[-1:] == HDR[:1] else bytearray()
                return
            if i: del self.buf[:i]              # 未归属字节（磁盘上 raw-dump 里可见）
            if len(self.buf) < 4: return
            n = (self.buf[2] << 8) | self.buf[3]
            if not (MIN_LEN <= n <= MAX_LEN): self.bad += 1; del self.buf[0]; continue
            if len(self.buf) < n: return
            f = bytes(self.buf[:n])
            if sum(f[:-1]) % 256 != f[-1]: self.bad += 1; del self.buf[0]; continue
            op, typ = f[6], f[7]
            mid = (f[4] << 8) | f[5]
            self.frames[(op, typ)] += 1
            self.frame_bytes += n
            if mid != self.last_msgid.get(op, mid):
                expect = (self.last_msgid[op] + 1) & 0xFFFF
                if mid != expect and mid != 0: self.msgid_gaps += 1
            self.last_msgid[op] = mid
            if self.last_frame_ts: self.max_gap = max(self.max_gap, now - self.last_frame_ts)
            self.last_frame_ts = now
            del self.buf[:n]
```

**判定阈值（30 分钟稳态窗口：打开后 T+10 s 起算，到 T+30 min）**：

| 指标 | 通过 | 有条件通过 | 失败 |
|---|---|---|---|
| 未归属字节率 = `(total − frame_bytes) / total` | **= 0**（首选方案的目标） | < 0.1% | ≥ 0.1% |
| 假同步/坏帧数 `bad` | **= 0** | ≤ 3 且全部发生在窗口起点 | > 3 或出现中文/ASCII 连续串 |
| `0x70` 帧率 vs 设定上报周期 | ±10% | ±20% | 超出或周期性掉帧 |
| `0x70` 最大帧间隔 | ≤ 3× 周期 | ≤ 5× 周期 | 更长（说明背压或 USB 卡顿） |
| `msg_id` 跳变次数 | 0 | ≤ 1/30 min 且有解释（模组侧重置） | 频繁跳变或回退 |
| 板子重启 | 无（raw-dump 中不出现新的启动文本，`msg_id` 不回到 0） | — | 出现 |

> 「丢帧率」定义：`(期望帧数 − 实收合格帧数) / 期望帧数`，其中期望帧数由「上报周期 × 时长」推算；且**必须**与「未归属字节率」同时看 —— 只丢字节不丢帧（帧被截断）与只丢帧不丢字节（模组没发）成因完全不同。

### 7.2 方法 B：与「雷达直连 USB-TTL」A/B 对照（交叉确认）

| 步骤 | 内容 |
|---|---|
| 1 | 拆下模组，用 3.3V USB-TTL（921600 8N1）直连模组 TX/RX/GND，PC 上跑同一脚本采集 **10 分钟** |
| 2 | 装回 H3，跑桥接固件，采集同样 **10 分钟**（同一个人、同一走位、同一工作模式） |
| 3 | 比对：① 总帧数（±0.5%）；② 总字节数（±0.5%）；③ `(op,type)` 直方图（每类 ±1 帧）；④ 最大帧间隔；⑤ `0x70` 帧内 `目标数` 的分布 |
| 4 | 若桥接侧系统性少帧 → 指向 USB 侧背压（背压点②）；若两侧同样少帧 → 模组侧行为，与桥无关 |

**通过阈值**：①② 差异 < 0.5%，③ 差异 ≤ 1 帧/类，④ 桥接 ≤ 1.2× 直连。满足即可判定「桥接对协议流是透明的」。

### 7.3 补充判据：洁净性专项（不依赖 30 min 长跑）

| 用例 | 步骤 | 通过 |
|---|---|---|
| 空载洁净 | 板子插 USB、**不开上位机**、脚本抓 60 s（模组若默认上报则会看到 0x70） | 未归属字节 = 0 |
| 打开串口洁净 | 脚本开/关串口 5 次 | 每次都能从第一个 `0x45 0x44` 起正常解析；不复位（`msg_id` 延续） |
| 上位机并发 | 桌面工具连上并点「开始追踪」，同时脚本抓包 | 出现 `0xA1/0xA0` 请求与 `0xA2` 应答，且 0x70 不断流；未归属字节 = 0 |
| 长跑 | 30 min | 见 7.1 阈值表 |

---

## 8. 联调步骤（含三层排障法）

### 8.1 正常流程（首次上板）

| 步 | 动作 | 期望现象 | 若不成立 |
|---|---|---|---|
| 1 | 模组插 **H3**（丝印 `RES A R T G V P`），USB-C 连 PC | 设备管理器出现「USB 串行设备 (COMx)」（VID:PID 303A:1001，以实测为准） | 见 8.2 第①层 |
| 2 | 烧 `merged-binary.bin` 到 **0x0**（`idf.py -p COMx write_flash 0x0 …` 或 `flash` 子命令） | 烧完自动复位 | — |
| 3 | 用 `tools/edv11p_frame_check.py --port COMx --minutes 1` 抓 1 分钟 | 未归属字节 = 0；若模组默认在模式 1 上报，应看到 `0xA3/0x70` | 见 8.2 第②层 |
| 4 | 用 `python human_track_v1.4_side_mount.py` 打开，选同一 COM，波特率 921600，点「连接」 | 100 ms 后自动发 `GET 0x02`，界面回读模块信息（`EDV11P_VB, Vx.y.z`） | 见 8.2 第②层（回读不到 = TX 方向不通） |
| 5 | 点「开始追踪」（SET 0x05=1） | 回读工作模式 1；随后 `0x70` 目标帧持续上报 | 见 8.2 第③层 |
| 6 | 设置上报频率（SET 0x10，界面默认 2 → 见 9-U1 的单位问题） | 帧率随之变化 | 见 8.2 第③层 |
| 7 | 人沿地面走一圈 | 轨迹与人数随实际变化，无明显跳变 | 见 9-U8（斜装几何参数） |
| 8 | 跑 30 min（7.1 阈值） | 见 7.1 通过口径 | — |

### 8.2 排障三层法（本项目 95% 现场问题适用，`agents.md`/`pitfalls` G 节的延续）

**第①层：电源（红灯 + EN 电平）**

| 现象 | 判据 | 处置 |
|---|---|---|
| 丝印 `RADAR` **红灯**不亮 | 3.3V_RADAR 没电 | 量 H3 的 `P`/`V` 脚对 GND 是否 3.3V；确认代码里 `edk60_radar_power_on()` 真的被调用（IO14 输出高）；注意 IO15 橙灯**只**是指示灯，**不能**用来判断供电（`edk60_board.h:130-146`） |
| 红灯亮但一点数据都没有 | 供电 OK，问题在数据线/波特率/排针 | 进第②层 |
| 红灯闪烁 / USB 掉枚举 | 供电能力不足（EDV11P 峰值标称 <1200 mA，见风险 R12） | 硬件核算 AP7362；必要时外部供电（**超出代理权限，升级用户**） |

**第②层：帧校验（PC 侧 raw-dump）**

| 现象 | 判据 | 处置 |
|---|---|---|
| raw-dump 全是 0 字节 | 没有数据到 PC | ①模组插的是 H3 不是 H4（H4 的 T 与 EN 同网，`pitfalls` G2）；②RX 配错脚（应 IO4）；③波特率≠921600（`pitfalls` E2：波特率不匹配会丢**全部**字节，看起来像没信号）；④模组未就绪（`PWR_READY_MS` 太短） |
| 有字节但 `0x45 0x44` 找不到 | 波特率不匹配 / 电平问题 | 用「已知可用」的 USB-TTL 直连模组对照（方法 B 步骤 1），先证明模组在发 |
| 帧头对、校验不过 | 字节被插入/丢失 | 若同时看到可打印文本 → 洁净性退化（回第 2 章 C1~C4）；否则怀疑 UART RX 缓冲溢出（加大 `UART_RX_BUF`） |
| 有 `0x45 0x44`、无后续 | 帧被截断（长度字段到达值超过实际字节数） | 抓包看重同步率；关注背压点① |

**第③层：工作模式 / 上报频率（协议语义）**

| 现象 | 判据 | 处置 |
|---|---|---|
| 只有应答、没有 `0x70` | 模组不在追踪模式，或上报频率=0 | `GET 0x05` 看是否 =1（协议 `:72` 默认 1，但可能被上次会话改过）；`GET 0x10` 看是否 =0（`:73` 设为 0 则不上报）；必要时 `SET 0x05=1`、`SET 0x10=2` |
| `0x70` 有但人数恒 0 | 感应区/屏蔽区配置把人挡住了，或挂高/检测高度与实际不符 | `GET 0x20/0x21/0x22/0x25/0x26` 回读；按侧装几何（`human_track_v1.4_side_mount.py:148-153` 的 tilted 方案：y 0~800 cm、挂高 200 cm）核对 |
| 有目标但坐标明显偏 | 斜装几何参数（倾角/挂高/离墙）与实际不符 | 进 T2-4 标定（风险 R9） |
| 完全无应答（连 `0xA2` 都没有） | **TX 方向不通**：IO5 未绑 / 被占用 | 检查 `APP_EDK60_EDV11P_TX_PIN=5`、`uart_set_pin` 是否传了真实脚号（对照第 1.2 节 4.3 的坑） |

---

## 9. 未确认项与需要上板验证的假设

| ID | 未确认项 | 当前证据 | 影响 | 验证方法 |
|---|---|---|---|---|
| **U1** | **`0x10`（上报频率）的单位**：协议文档写 **50 ms**，任务书基线写 **10 ms** | `docs/EDV11P串口通用协议.md:73`「单位：50ms」；上位机 `human_track_v1.4_side_mount.py:1562`「单位: 50ms, 0表示不上报」，默认值 2 | 直接决定帧率与背压估算（本方案两种口径都算过：10 ms 也只需 55.6% 线速，不影响可行性）；但影响第 7 章期望帧数 | 上板 `SET 0x10=2` 后用脚本时间戳统计实际帧间隔：100 ms ⇒ 50 ms 单位成立；20 ms ⇒ 10 ms 成立 |
| **U2** | 模组型号分支（-02 人数统计 vs -03 跌倒+人存）是否支持 `0x70` | 风险登记册 R11（P=3, I=5） | 若不支持，则整条演示链路「有数据但无轨迹」 | 上板第一步 `GET 0x02`，比对回读设备 ID/固件版本；必要时改烧 V121_SX |
| **U3** | 模组出厂**默认工作模式与默认上报频率** | 协议 `:72` 写「默认：1」；`:73` 未写默认值 | 决定「不注入任何命令时是否直接有 0x70」 | 烧固件后**不打开上位机**，脚本抓 60 s：若出现 0x70 ⇒ 默认即上报；若只有静默 ⇒ 必须靠上位机 SET |
| **U4** | 上电到模组可输出的真实时间 | 任务书/文档均无实测值；`PWR_READY_MS` 默认 800 ms 是保守猜测 | 太短 ⇒ 丢掉最初几帧（对 30 min 统计影响可忽略，但会让「干净起见的第一帧」判定失真） | 示波器测 H3 `P` 脚上升沿到 IO4 出现第一个 `0x45` 的时间；连续 10 次取 max |
| **U5** | 3.3V_RADAR（AP7362-33SP-13）能否承受 EDV11P 峰值电流 | 风险 R12 已登记；本方案未核算 | 峰值掉压 ⇒ 模组重启 / USB 掉枚举 | 示波器抓 H3 `P` 脚在上电瞬间的跌落深度与恢复时间；同时观察 USB 是否掉枚举 |
| **U6** | IO4/IO5 同时是 ADC1_CH4/CH5（`edk60_board.h:159-160`）与 strapping 脚（`:229-230`），做 921600 UART 时是否受 10R 隔离电阻/外部电路影响 | 现有 5.1 已在 IO4 收到合法帧（`pitfalls` G1），说明 RX 没问题；**TX（IO5）在本工程从未被真正使用过**（4.3 没绑、5.1 只用 RX） | TX 若波形质量差 ⇒ 上位机 SET 全部无效 | 示波器/逻辑分析仪测 IO5 波形（921600 下上升沿/占空比）；或直接看 `GET 0x02` 是否有 `0xA2` 应答 |
| **U7** | 打开 COM 口是否会复位 C6（原生 USJ 是否实现 DTR/RTS 自动复位） | `pitfalls` D3 是裸 UART-桥接板的经验；原生 USJ 无外部自动复位电路，但 Qt/pyserial 默认会置位 DTR/RTS，模组/ROM 侧解释未知 | 若会复位 ⇒ 每次连接丢前 1~2 s，且 raw-dump 开头会出现启动文本（影响「洁净」判定） | 脚本故意开/关串口 5 次，观察 `msg_id` 是否从 0 重新开始 |
| **U8** | 斜装几何（倾角 60°、挂高 200 cm、y 0~800）与实际安装的匹配度 | 上位机 `:148-153` 内置两套 profile；实际安装未标定 | 轨迹系统性偏移（风险 R9） | T2-4：地面画十字，人沿固定直线走，比对轨迹偏差 < 30 cm |
| **U9** | 单帧真实最大长度是否可能超 512 B（10 感应区 × 10 目标 ⇒ 值 720 B） | `edv11p.h:32` 定义 `MAX_FRAME_LEN 512`；协议 `:91` 允许「nByte」 | 若真超 512，`CHUNK=512` 会把一帧拆成两次 USB 写（**对透传无害**，但会让第 7 章脚本的 `MAX_LEN` 判据误报坏帧） | 抓包统计实测最大帧长；必要时脚本把上限设为 1024 并只对 ≤512 的帧做严格校验 |
| **U10** | USB TX 8192 B 是否足以覆盖「PC 侧卡顿 300 ms」 | 理论 160 ms 余量（@51.2 kB/s） | 卡顿时丢字节 ⇒ 坏帧 | 方法 A 长跑 + 人为制造卡顿（脚本里 `time.sleep(0.5)`）观察 `usb_tx_dropped` 计数 |
| **U11** | 关掉 `CONFIG_APP_EDK60_STARTUP_BANNER`/`PRINT_PINMAP` 后，`demo_dispatch.c` 现有 `#if` 是否会产生 `-Wundef` 警告（本工程 warning 当门禁） | `build-radar_bridge/config/sdkconfig.h` 证实未选中项**不生成宏** | 若报警告 ⇒ 构建门禁失败 | M3 改 `#ifdef` 后全量 `build_all.ps1 -Size` 双向验证（17/17 且 0 warning） |
| **U12** | `CONFIG_FREERTOS_HZ=1000` 是否引入副作用（功耗/中断开销） | 默认 100 Hz（`freertos/Kconfig` `default 100`） | 仅本 demo 生效；理论上 CPU 中断开销 +~0.1% | 对比 30 min 帧统计与直连对照（方法 B），差异应 < 0.5% |

> **合计 12 条未确认项**，其中 U1/U2/U3 影响「能不能出轨迹」，U4/U5/U6/U9 影响「上板能不能一次通」，U7/U8/U10/U11/U12 影响「判定口径是否可信」。

---

## 附录 A：一句话交接（给实现者）

> 新建 `radar_bridge_edv11p`：UART1（IO4 RX / IO5 TX / 921600 8N1，EN=IO14 常高）⇄ 原生 USJ 双向纯字节搬运；**不注入任何帧、不打任何字符**；控制台保持 UART0 + USB 次级**不动**，靠 `LOG_DEFAULT_LEVEL_NONE` + 关横幅/引脚表 + 源码零 `printf`（诊断改直写 UART0 且默认关闭）实现洁净；构建走 `build_all.ps1 -Demo radar_bridge_edv11p`，产物 `merged-binary.bin` 烧 0x0；验收用 30 min 帧校验 + 直连 A/B 对照。

## 附录 B：本文引用的外部文件（只读，未修改）

| 文件 | 用途 |
|---|---|
| `docs/EDV11P串口通用协议.md`（registry/ed_radar，V1.9） | 帧结构、op/type、默认值 |
| `...\ed_radar\radar\edv11p.h` / `edv11p.c` | `MAX_FRAME_LEN=512`、`MIN_FRAME_LEN=11` 交叉核对（本 demo 不引入该组件） |
| `C:\Users\fuhua\Desktop\human_track_v1.4_side_mount.py` | 上位机行为（连接时序、解析器重同步、上报频率单位） |
| `C:\esp\v6.1\esp-idf\components\esp_stdio\Kconfig`、`stdio_vfs.c`、`stdio_simple.c` | 控制台扇出机制、`ESP_CONSOLE_*` 符号 |
| `C:\esp\v6.1\esp-idf\components\esp_driver_usb_serial_jtag\Kconfig`、`src\usb_serial_jtag_vfs.c` | `USJ_ENABLE_USB_SERIAL_JTAG`、次级模式逐字符写 FIFO |
| `C:\esp\v6.1\esp-idf\components\esp_driver_uart\include\driver\uart_vfs.h` | `uart_vfs_dev_use_driver()` 的必要性 |
