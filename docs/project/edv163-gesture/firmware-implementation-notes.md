# EDV163 挥手识别透传桥固件实现说明（demo 5.4 `radar_bridge_edv163_gesture`）

> 角色：嵌入式/软件工程师（opcs-sw-engineer）｜技能：emb-driver、emb-arch
> 最后更新：2026-10-23
> 工程根：`C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos`
> 目标：ESP32-C6-MINI-1-N4（4 MB Flash / 无 PSRAM）、ESP-IDF **v6.1.0**、target `esp32c6`
> 事实来源：模组厂商上位机 `gesture_monitor.py`（774 行）+ 其 README（无协议文档，全部逆推）

---

## 一、这个 demo 是什么（30 秒版）

| 项 | 内容 |
|---|---|
| 干什么 | 把 EDV163 挥手识别模组（固件 `163_action_20260901`，插 **H3**）的串口数据以 **115200 8N1** 收进来，**原样**搬到 ESP32-C6 原生 USB Serial/JTAG |
| 不干什么 | **不解析协议、不切行、不识别挥手、不跳过二进制帧、不发任何命令、不做转义/反转义、不改写任何字节**（不加换行、不加前缀、不 hexdump、不 trim、不统一 CRLF） |
| 引脚 | UART1 RX=**IO4**（模组 TX）、TX=**IO5**（模组 RX）、雷达电源 EN=**IO14**（拉高常驻）、橙灯 IO15 **完全不碰** |
| 上位机 | 打开「USB 串行设备 (COMx)」，波特率填 115200（USB CDC 不受波特率限制，填一致只为对数） |
| 入口 | `demo_radar_bridge_edv163_gesture_start()`，由 `demo_dispatch.c` 在选中 `APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE` 时调用 |
| 协议归属 | 文本行 `GESTURE: target=… x=… y=… z=… action=…`、`action` 编码（含 ±3 靠近/远离）、0x4544 点云帧的「长度 + 累加校验 + 失败只丢 1 字节」重同步规则 —— **全部在上位机侧**。固件里没有任何一行对应代码（源码头部只以注释形式留了一份协议速查，供评审看清边界） |
| 数据方向 | 上位机**只读不写**（厂商源码里没有任何 `.write(`）。固件的 USB→UART 通路保留但**常态空闲**，且**永远不自己造字节** |

### 1.1 交付物清单（全部为本次改动）

| 文件 | 性质 | 说明 |
|---|---|---|
| `components/edk60_app/src/demo_get_started_radar_bridge_edv163_gesture.c` | **新增** | 全部实现（816 行，含方案说明、定量算式、混流论证、上板验证、两条编译期断言） |
| `sdkconfig.defaults.radar_bridge_edv163_gesture` | **新增** | demo 专用 defaults（静默 + 115200 + 关横幅/引脚表） |
| `main/Kconfig.projbuild` | 修改 | choice 加 `APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE`；新增菜单「EDK60 Demo 参数（EDV163 手扫透传桥 5.4）」共 7 项 |
| `components/edk60_app/src/demo_dispatch.c` | 修改 | 新增 `#elif` 分支 + 入口前置声明 + **横幅门禁扩展到本 demo** |
| `components/edk60_app/CMakeLists.txt` | 修改 | `EDK60_DEMO_SRCS` 增加一行（不登记则源文件不参与编译） |
| `build_all.ps1` | 修改 | `$DemoMap` **仅加一行**：`'radar_bridge_edv163_gesture' = 'APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE'` |
| `docs/project/edv163-gesture/firmware-implementation-notes.md` | **新增** | 本文 |

> **未改动 / 未删除任何文件**：其余 18 个 demo 源文件、5.2/5.3 的源码与 defaults、被 19 个 demo 共用的 `edk60_demo.h` 一字未动
> （`edk60_demo.h` 按 5.2/5.3 的既有做法只在 `demo_dispatch.c` 里前置声明，把影响面收在一个文件内）。
> 5.3 在新配置下仍能编译，见第五节的用例 8/9。

---

## 二、与 5.3（水位）的三处**实质**差异

| # | 维度 | 5.3 水位（`radar_bridge_edv163`） | **5.4 手扫（本 demo）** | 后果 / 处置 |
|---|---|---|---|---|
| 1 | **波特率** | 256000（25600 B/s） | **115200**（**11520 B/s**） | 线速只有 5.3 的 45%。所有「每秒多少字节 / 多少毫秒的缓冲深度」结论**必须按 11520 B/s 重算**（见第四节）。波特率错一档的现场现象是**一个字节都收不到**（帧错误让驱动整体丢字节，pitfalls E2），不是乱码 → 上板第一件事是数「每秒几行 `GESTURE:`」（应约 5 行/秒） |
| 2 | **数据形态** | 纯二进制帧（帧头 0x4544 + len + checksum + 帧尾 0x0A + 转义） | **文本行（主体）+ 偶发 0x4544 二进制点云帧（混流）** | 这是**最需要改变思维方式**的一条：文本行没有长度字段、没有校验，被污染后**没有任何"校验失败"信号**，只表现为"坏行/挥手方向偶尔不显示"。完整论证见第三节 |
| 3 | **写入需求** | 上位机下发 GET/SET（0x18 工作模式、0x17 上报频率、0x25 挂高…） | **完全只读**（厂商源码无 `.write(`，模组无命令接口） | 固件"零命令注入"从"我们不生成命令"升级为"链路本身没有命令"。USB→UART 通路保留（桥的语义是双向管），但常态为空、且绝不自己造数据 |

### 2.1 命名与参数差异（照抄上一版最容易漏的地方）

| # | 项目 | 5.3 写法 | 5.4 实际 | 说明 |
|---|---|---|---|---|
| 4 | 文件名 | `demo_get_started_radar_bridge_edv163.c` | `demo_get_started_radar_bridge_edv163_gesture.c` | — |
| 5 | 入口函数 | `demo_radar_bridge_edv163_start()` | `demo_radar_bridge_edv163_gesture_start()` | `demo_dispatch.c` 前置声明 + `#elif` 分支 |
| 6 | Kconfig 前缀 | `APP_EDK60_EDV163_*` | **`APP_EDK60_EDV163_GESTURE_*`** | 7 项全部改名，**不复用 5.3 的符号**（复用会让两个 demo 的参数互相影响） |
| 7 | Kconfig 选择项 | `APP_EDK60_DEMO_RADAR_BRIDGE_EDV163` | `APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE` | 两者是不同宏，`demo_dispatch.c` 的横幅门禁把**三个**桥 demo 一起排除 |
| 8 | 文件内宏前缀 | `BR163_*` | **`BRGEST_*`** | `grep BRGEST_` 不会串到 5.3 |
| 9 | 日志 TAG | `"bridge163"` | **`"bridgegest"`** | 仍不以 `E`/`D` 开头（假帧头防护），且静默模式下不输出 |
| 10 | 统计任务名 | `"brstat"` | `"brgstat"` | 便于 `task list` 区分 |
| 11 | 菜单标题 | 「EDK60 Demo 参数（EDV163 透传桥 5.3）」 | 「EDK60 Demo 参数（**EDV163 手扫透传桥 5.4**）」 | — |
| 12 | demo 编号 | 5.3 | **5.4** | 工程从 18 个 demo 变成 **19 个**（`build_all.ps1 -List` 已实测 19） |
| 13 | 诊断文案 | `demo=radar_bridge_edv163` | `demo=radar_bridge_edv163_gesture` | 参数行内容按 115200 重算 |

### 2.2 刻意**保持**不变的部分（改动会引入风险，故不动）

| 项 | 取值 | 理由 |
|---|---|---|
| console 配置 | 不动（主控制台 UART0 + USB 第二输出口） | 现状即目标状态；改动会踩 D1 启动卡死 |
| USB 写单批预算 | **60 ms** | 与 5.2/5.3 同一套「有界停摆 + 计数」模型，余量从 10.67× 放大到 23.7× |
| UART RX 缓冲 | 16384 B（Kconfig 可调 1~32 KB） | 本板 SRAM 512 KB；且本 demo 单帧最长 4096 B（355 ms 线时），缓冲越深越安全 |
| USB TX / RX 缓冲 | 16384 B / 4096 B | 同上；RX 侧本 demo 常态为空，纯预留 |
| 任务结构 | `r2u`/`u2r` 优先级 6、栈 3072；统计任务优先级 3 且仅诊断时创建 | 已验证结构 |
| 单次搬运块 | 1024 B | 因为**不攒整帧**，块大小无需与帧边界对齐（详见 4.2） |
| EN 处理 | 上电后常驻高，运行期不碰 | 历史坑：运行中误拉低 = 雷达掉电 |
| 不延时等模组启动 | 立即开始搬运 | 等待期间的数据会白丢（UART 已装好、缓冲已在收） |

---

## 三、为什么「文本 + 二进制混流」**更**不能碰字节流（本次核心论证）

前两个桥 demo 的流是「纯二进制帧」，帧头 0x4544 自带同步：即使桥上多吐一两个字符，
上位机的帧同步最多丢一帧就能重新对上，属**局部可恢复**。
本 demo 的流是「文本行 + 偶发二进制帧」，脆弱性完全不同：

| # | 事实（来自厂商源码） | 如果桥上做一点点处理会怎样 |
|---|---|---|
| ① | 文本行**没有长度字段、也没有校验**；上位机的锚点是行首 `^GESTURE:` 与行尾 `\n` | 多插一个字符（哪怕只是 `\r` 或空格）⇒ 该行**匹配失败**；少一个字符 ⇒ 与后续字符粘成一行。文本协议**没有任何校验能发现**这种错误 |
| ② | 解析器遇到认不出的行直接返回 `None`（静默丢弃） | 桥上一旦污染，现场只会看到「挥手方向偶尔不显示」，**绝不会**看到"校验失败"这种明确信号 —— 最难查的一类故障 |
| ③ | 厂商的切流规则是「找 `\n` 与 0x4544 谁先出现」+「长度范围 11~4096」+「累加校验」+「**校验失败只丢 1 个字节**重新同步」四件事一起做 | 桥上若"顺手过滤 0x4544 帧"：<br>· 文本里出现字节序列 0x4544（未来固件加字段就可能）无法区分；<br>· 二进制帧**载荷里**也常有 0x4544，按帧头解析会把载荷当帧头、长度字段变垃圾 ⇒ 删掉的是**真数据**；<br>· 这四条规则合起来**已经就是一个完整协议解析器**，把它搬进固件等于把协议知识复制两份（固件 + 上位机），两份必然漂移 |
| ④ | 模组固件可升级（本次就是从 5.3 的 256000 水位固件换到 115200 挥手固件） | 桥上开始"理解"后，模组换固件就必须连带改桥、重烧、重新认证。而现在桥只搬字节，模组换固件**不需要动 ESP32** —— 本次换机型只改了波特率一行宏 |

**⇒ 设计铁律：桥只允许改变「字节到达的时刻」，不允许改变「字节本身」。**
任何「顺手做个文本处理」的念头都必须回到这一条上否掉。

**⇒ 由此推出洁净要求升级**：前三个 demo 里"日志污染"只是让上位机跳过几帧；
本 demo 里它直接吃掉**文本行的完整性**，所以静默不是"锦上添花"，是**功能正确性的一部分**。

---

## 四、缓冲区 / 超时 / 背压：按 115200 重算（含编译期断言）

线速：**115200 8N1 = 10 bit/byte ⇒ 11520 B/s**。
1024 B 线上时间 ≈ **88.9 ms**；单个最大二进制帧 **4096 B ≈ 355.6 ms**；
文本基线 ≈55–60 B/行 × 5 行/秒 ≈ **280–300 B/s**（约 2.4–2.6% 线速，真正吃带宽的是点云帧）。

| 参数 | 取值 | 依据（按 11520 B/s 算） | 与 5.3 对比（25600 B/s） |
|---|---|---|---|
| UART1 RX 环形缓冲 | **16384 B**（`APP_EDK60_EDV163_GESTURE_UART_RX_BUF`，1024~32768） | 16384 ÷ 11520 = **1422.2 ms** 的流量 | 640.0 ms |
| USB 写单批总预算 | **60 ms**（单次尝试 10 ms、退避 2 ms） | 60 ms × 11520 B/s = **691 B** 的最坏积压 | 1536 B |
| **比值（唯一的定量保证）** | **23.7×** | 1422 ms ÷ 60 ms ⇒ 主机 60 ms 内恢复读取就一个字节不丢 | 10.67× |
| USB TX 缓冲 | 16384 B | ≈ 1422 ms 的突发吸收量（与 UART 侧叠加 ≈ 2.8 s） | ≈ 640 ms |
| USB RX 缓冲 | 4096 B | 本 demo 上位机不发字节，纯预留 | 配置帧 2~4 KB |
| UART1 TX 缓冲 | 4096 B | `uart_write_bytes` 阻塞到全部入队 ⇒ 天然反压 | 相同 |
| 单次搬运块 | **1024 B**（`APP_EDK60_EDV163_GESTURE_CHUNK`，64~2048） | ≈89 ms 线时；**不攒整帧**，故无需与帧边界对齐 | 相同 |
| 读超时 | UART/USB 各 **10 ms** | 11.5 KB/s 下 10 ms 只积 ~115 B | 相同 |
| UART 事件队列 | 32 项 | 数据走环形缓冲；队列只用来统计「满/溢出/帧错」 | 相同 |
| RAM 增量 | **≈42 KB** | 16384+4096+16384+4096+2×1024 = 43008 B | 相同 |

### 4.1 编译期断言（两条都**绑定本 demo**）

源码 **L798~L801**（禁止关闭 USB 第二控制台）与 **L812~L816**（缓冲/波特率不等式）：

```c
#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE) && \
    ((CONFIG_APP_EDK60_EDV163_GESTURE_UART_RX_BUF * 1000) / \
     (CONFIG_APP_EDK60_EDV163_GESTURE_BAUD / 10) < (2 * 60))
#error "… UART_RX_BUF×1000÷(BAUD/10) 必须 ≥ 120 ms（= 2×USB 写预算 60 ms）…"
#endif
```

- 默认组合：16384×1000÷11520 = **1422 ms ≥ 120 ms** ⇒ 通过。
- **115200 下这条断言仍然有牙齿**：1024 B 只有 88 ms（1024000÷11520 = 88）< 120 ms ⇒ 构建直接失败；
  最小可用值 **2048 B（177 ms）**。这一点已被第五节用例 5 实测证明。
- 两个断言的第一条件都是 `defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE)`，
  所以对其它 18 个 demo **完全无效**（用例 3 实测：未选中 + 同一套敌对配置，exit 0）。

### 4.2 可观测计数器（默认关闭的诊断通道输出，判据写在输出行里）

| 计数器 | 来源 | 含义 |
|---|---|---|
| `uart_rx_overflows` | `UART_BUFFER_FULL` | 环形缓冲满 ⇒ **有字节被驱动丢弃** |
| `uart_fifo_ovf` | `UART_FIFO_OVF` | 硬件 FIFO 溢出 |
| `uart_frame_errs` | `UART_FRAME_ERR` | **波特率不对的典型症状**（本 demo 尤其要盯） |
| `usb_tx_dropped` / `usb_tx_short` | `usb_write_all()` 返回值 < 本批长度 | 因 USB 写超预算放弃的字节数 / 批次 |
| `usb_wr_stalls` | `usb_serial_jtag_write_bytes()` 返回 ≤0 | 写受阻次数 |

**判据：`uart_rx_overflows == 0` 且 `usb_tx_dropped == 0` 且 `uart_frame_errs == 0`** ⇒ 零丢字节且波特率正确。
实现位置：`usb_write_all()` L476、`uart_drain_events()` L509、`task_radar_to_usb()` L559、`task_stats()` L621。

---

## 五、控制台洁净做法（四道保险 + 一道门禁，与前三个 demo 同构）

| 层 | 做法 | 位置 | 生效范围 |
|---|---|---|---|
| a) 编译期 | `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` ⇒ `ESP_LOG*` 被编译成空语句 | `sdkconfig.defaults.radar_bridge_edv163_gesture` | 全固件 |
| b) 编译期 | `# CONFIG_APP_EDK60_STARTUP_BANNER is not set` + `# CONFIG_APP_EDK60_PRINT_PINMAP is not set` ⇒ 两处 printf 不执行 | 同上 | 本 demo |
| c) 源码 | 本文件**通篇没有 printf、没有 ESP_LOGx**（唯一例外是默认关闭的诊断通道，见 d）；全文件仅 1 处 `vsnprintf` 且位于 `#if BRGEST_DBG_ENABLED` 内 | 新源码 | 本 demo |
| d) 运行期 | demo 第一行就 `esp_log_level_set("*", ESP_LOG_NONE)`，兜住「用户自己 menuconfig 编、没走 demo 专用 defaults」 | `demo_radar_bridge_edv163_gesture_start()` L653 起 | 全固件 |
| e) **门禁（双保险）** | `demo_dispatch.c` 里整块跳过「横幅 + 复位原因 + 引脚表」，**不依赖 sdkconfig 是否记得关** | `#if !defined(...EDV11P) && !defined(...EDV163) && !defined(...EDV163_GESTURE)` | 本 demo |

诊断通道（默认关闭）：`CONFIG_APP_EDK60_EDV163_GESTURE_UART0_DEBUG=y` 时走
`esp_rom_output_tx_one_char()` **直写 UART0**（H2 的 D1/IO17，115200），不经过 stdio，
因此不会被复制到 USB（依据 `components/esp_stdio/stdio_simple.c`：复制发生在 stdio 层）。
文案刻意不使用以 `"E""D"` 开头的词（用 `k60` / `radar` 代替）。
`CONFIG_APP_EDK60_EDV163_GESTURE_STATS_VIA_USB=y` 是**仅供一次性取证**的反向开关
（故意污染数据流，用来在没有 UART0 串口板时读背压计数器），正式固件必须保持 `n`。

**本 demo 新增的门禁实证**（不是"读代码认为对"，而是预处理结果）：
`STARTUP_BANNER=y + PRINT_PINMAP=y + 选中 5.4` 时，`gcc -E demo_dispatch.c` 的结果里
`edk60_print_banner` / `edk60_print_reset_reason` / `edk60_print_pinmap` **各只剩 1 处**
（即 `edk60_board.h` 里的原型），**调用点全部消失**；
同样配置下 `hello_board` 是各 2 处（原型 + 调用）。见第六节证据 E3。

---

## 六、编译期验证（本会话不能跑 `idf.py`，这是等价替代）

### 6.1 为什么不能跑构建（环境事实）

本机沙箱禁止 `idf.py` 使用的**命名管道**：`asyncio → windows_utils.pipe() → _winapi.CreateFile`
直接 `PermissionError: [WinError 5] 拒绝访问`。与 5.2/5.3 当时的记录逐条一致。
因此用两条纯用户态路径做等价验证：**kconfgen**（Kconfig/defaults 叠加）与
**`-fsyntax-only`**（真实编译参数 + `-Werror`）。真正的 `idf.py build` 由项目经理执行（见 6.4）。

### 6.2 Kconfig + defaults 叠加（kconfgen，6 个场景全 exit 0）

命令形态（与 5.3 完全一致；`--config` 指定的文件必须**不存在**，才会从 defaults 全新生成）：

```powershell
& $py -m kconfgen --kconfig "C:/esp/v6.1/esp-idf/Kconfig" --env-file "<tmp>\config.env" `
    --env "IDF_ENV_FPGA=" --env "IDF_INIT_VERSION=v6.1" --config "<tmp>\sdkconfig_cfg_gest_main" `
    --defaults "<repo>\sdkconfig.defaults" `
    --defaults "<tmp>\demo_gest" `
    --defaults "<repo>\sdkconfig.defaults.radar_bridge_edv163_gesture" `
    --output config "<tmp>\sdkconfig_cfg_gest_main" --output header "<tmp>\cfg_gest_main\sdkconfig.h"
```

| 场景 | defaults 叠加 | exit | 说明 |
|---|---|---|---|
| `cfg_gest_main` | 工程 defaults + `DEMO_..._GESTURE=y` + **demo 专用 defaults** | **0** | 量产配置 |
| `cfg_gest_dbg` | ＋ `UART0_DEBUG=y` | **0** | 诊断通道 |
| `cfg_gest_other` | 工程 defaults + `DEMO_HELLO_BOARD=y` + **敌对配置** | **0** | 未选中本 demo |
| `cfg_gest_bad` | 工程 defaults + `DEMO_..._GESTURE=y` + **敌对配置** | **0** | 断言反向验证用 |
| `cfg_gest_usbstats` | ＋ `STATS_VIA_USB=y` | **0** | 取证通道 |
| `cfg_gest_nosilent` | ＋ `SILENT_LOGS=n` | **0** | `#ifdef` 兜底路径 |
| `cfg_gest_banner`（附加） | ＋ `STARTUP_BANNER=y` + `PRINT_PINMAP=y` | **0** | 门禁实证用 |

`cfg_gest_main` 生成结果（**关键行**，逐字摘录）：

```
CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE=y
# CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163 is not set
# CONFIG_APP_EDK60_STARTUP_BANNER is not set
# CONFIG_APP_EDK60_PRINT_PINMAP is not set
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
CONFIG_APP_EDK60_EDV163_GESTURE_BAUD=115200
CONFIG_APP_EDK60_EDV163_GESTURE_SILENT_LOGS=y
# CONFIG_APP_EDK60_EDV163_GESTURE_UART0_DEBUG is not set
# CONFIG_APP_EDK60_EDV163_GESTURE_STATS_VIA_USB is not set
CONFIG_APP_EDK60_EDV163_GESTURE_STATS_SEC=10
CONFIG_APP_EDK60_EDV163_GESTURE_UART_RX_BUF=16384
CONFIG_APP_EDK60_EDV163_GESTURE_CHUNK=1024
```

对应 `sdkconfig.h`（**只有 6 个参数宏**，两个 `default n` 的 bool 按硬规则 A2 **不生成宏**，
所以 C 侧必须 `#ifdef`，代码已照此写）：

```c
#define CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE 1
#define CONFIG_APP_EDK60_EDV163_GESTURE_BAUD 115200
#define CONFIG_APP_EDK60_EDV163_GESTURE_SILENT_LOGS 1
#define CONFIG_APP_EDK60_EDV163_GESTURE_STATS_SEC 10
#define CONFIG_APP_EDK60_EDV163_GESTURE_UART_RX_BUF 16384
#define CONFIG_APP_EDK60_EDV163_GESTURE_CHUNK 1024
```

**demo 选择计数**（同一份 sdkconfig）：`已选 = 1`（`..._GESTURE=y`）、`未选 = 18`
⇒ choice 成员 **18 → 19** 已确认；`build_all.ps1 -List` 也实测打印「共 19 个」，且第 19 行是
`radar_bridge_edv163_gesture -> CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE`。

### 6.3 `-fsyntax-only`：10 个用例全过（4 个必测 + 6 个附加）

编译参数**取自真实构建**（`build-hello_board/compile_commands.json` 里 `radar_bridge_edv163` 那一行，
脚本先断言该命令行确实含 `-Wall -Wextra -Werror -std=gnu23`），只做两处替换：
① `-I…/build-hello_board/config` 换成对应场景的 `sdkconfig.h` 目录；② `-o …obj` 换成 `-fsyntax-only`。

| # | 场景 | 源文件 | 配置 | 结果 |
|---|---|---|---|---|
| **1** | **量产配置**（选中本 demo、静默、无诊断） | 新源码 | `cfg_gest_main` | **exit 0，输出 0 字节**（0 error / 0 warning） |
| **2** | **诊断开关打开**（`UART0_DEBUG=y`，dbg 分支参与编译） | 新源码 | `cfg_gest_dbg` | **exit 0，输出 0 字节** |
| **3** | **未选中本 demo + 敌对配置**（`CONSOLE_SECONDARY_NONE=y` + 3 Mbps + 1024 B 缓冲） | 新源码 | `cfg_gest_other` | **exit 0，输出 0 字节** ⇒ 两条 `#error` **不泄漏**到其它 18 个 demo |
| **4** | **`demo_dispatch.c` 选中本 demo**（走新 `#elif` + 横幅门禁） | `demo_dispatch.c` | `cfg_gest_main` | **exit 0，输出 0 字节** |
| 5 | **断言反向验证**：选中本 demo + 同一套敌对配置 | 新源码 | `cfg_gest_bad` | **exit 1**，两条 `#error` **同时命中** ⇒ 断言不是死代码 |
| 6 | 取证通道 `STATS_VIA_USB=y` | 新源码 | `cfg_gest_usbstats` | **exit 0，输出 0 字节** |
| 7 | `SILENT_LOGS=n`（`#ifdef` 兜底路径，此时宏根本不存在） | 新源码 | `cfg_gest_nosilent` | **exit 0，输出 0 字节** |
| 8 | **5.3 源文件**在 5.4 配置下（确认没被本次改动影响） | `..._edv163.c` | `cfg_gest_main` | **exit 0，输出 0 字节** |
| 9 | 5.3 源文件在「未选中 + 敌对配置」下（其断言也不泄漏） | `..._edv163.c` | `cfg_gest_other` | **exit 0，输出 0 字节** |
| 10 | `demo_dispatch.c` 走 hello_board 路径（其它 demo 回归） | `demo_dispatch.c` | `build-hello_board/config` | **exit 0，输出 0 字节** |

用例 5 的实测输出（摘录，证明两条断言都真的会炸）：

```
demo_get_started_radar_bridge_edv163_gesture.c:800:2: error: #error "radar_bridge_edv163_gesture 不允许用「关闭 USB 第二控制台」来让数据流变干净…"
demo_get_started_radar_bridge_edv163_gesture.c:815:2: error: #error "radar_bridge_edv163_gesture 的缓冲区/波特率组合不安全：UART_RX_BUF×1000÷(BAUD/10) 必须 ≥ 120 ms…"
```

**证据 E3（洁净门禁）**：用 `-E` 看预处理结果，`STARTUP_BANNER=y + 选中 5.4` 时
`edk60_print_banner/reset_reason/pinmap` 各只剩原型（1 处），`hello_board` 同配置下各 2 处（原型+调用）。
另外 `cfg_gest_main` 下 `gcc -E demo_dispatch.c` 里 **`"启动 Demo"` 字面量出现 0 次**
（`CONFIG_LOG_DEFAULT_LEVEL_NONE` 把 `ESP_LOGI` 整体消掉），全文件唯一的 `printf` 命中是
libc 头文件里的原型、不是调用 ⇒ **"零 printf" 有证据，不是声明**。

### 6.4 复现脚本位置（**未放进仓库**，请 PM 决定去留）

本次验证脚本放在临时目录，**没有往 `tools/` 里新增文件**（严格遵守本次"只动指定文件"的约束）：

| 脚本 | 作用 |
|---|---|
| `%TEMP%\kcfggest_run.ps1` | kconfgen 7 场景 + 关键项核对 + 成员计数 |
| `%TEMP%\syntax_check_edv163_gesture.ps1` | `-fsyntax-only` 10 用例 |
| `%TEMP%\gate_check_b.ps1` | 门禁 `-E` 实证 |

如需长期回归，建议 PM 授权把它们合并为 `tools/syntax_check_edv163_gesture.ps1`（与既有的
`tools/syntax_check_edv163.ps1` 同一形态）。**未经授权我没有在仓库里创建它。**

### 6.5 没有被语法检查覆盖的部分（诚实列出）

| 项 | 是否覆盖 | 说明 |
|---|---|---|
| C 源文件语法/告警 | ✅ | 10 用例、`-Werror` 全绿、输出 0 字节 |
| Kconfig 语法 + defaults 叠加 | ✅ | kconfgen 7 场景 exit 0，逐项核对 |
| `demo_dispatch.c` 分派与门禁 | ✅ | 用例 4/10 + `-E` 实证 |
| 组件 `CMakeLists.txt` 源文件登记 | ⚠️ 部分 | 已加进 `EDK60_DEMO_SRCS`；`list(LENGTH)` 待构建时确认 **19** |
| 链接期符号裁剪（`--gc-sections`） | ❌ | 需真实构建 + `nm` |
| 固件体积 / 分区余量 | ❌ | 需真实构建 |
| 上板行为（行率/坏行率/二进制帧完整性/背压计数） | ❌ | 需上板实测 |

### 6.6 需要项目经理执行的命令（按顺序）

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos

# 1) 单 demo 构建（会先删 sdkconfig.radar_bridge_edv163_gesture 再按 defaults 重新生成）
.\build_all.ps1 -Demo radar_bridge_edv163_gesture

# 2) 全量回归（19/19；重点确认其余 18 个 demo 没被新断言/新 Kconfig 影响）
.\build_all.ps1 -Size

# 3) 合并固件 + 体积 + SHA256（发布用，烧 0x0）
idf.py -B build-radar_bridge_edv163_gesture merge-bin
Get-Item build-radar_bridge_edv163_gesture\merged-binary.bin | Select-Object Length
Get-FileHash build-radar_bridge_edv163_gesture\merged-binary.bin -Algorithm SHA256
```

**回填口径**：把 1) 的 `OK/体积`、2) 的 `19/19`、3) 的体积与 SHA256 填回本文第六节与
`docs/project/edv163-gesture/` 的 Gate 记录。

可选（验证符号裁剪与配置真的生效）：

```powershell
# 选中本 demo 的宏应存在、5.3 的 demo 宏应不存在
Select-String build-radar_bridge_edv163_gesture\config\sdkconfig.h `
    -Pattern "DEMO_RADAR_BRIDGE_EDV163|EDV163_GESTURE_BAUD"
# 未选中 demo 的入口符号应被 --gc-sections 丢掉（应无输出）
& C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin\riscv32-esp-elf-nm.exe `
    build-radar_bridge_edv163_gesture\edk60_demos.elf | Select-String "demo_radar_bridge_edv163_start"
```

---

## 七、上板后应看到什么（验收口径）

| 阶段 | 期望现象 | 判据 / 怎么确认 |
|---|---|---|
| 上电 0~1 s | 丝印 **RADAR 红灯亮**（= IO14 已拉高）；PC 出现「USB 串行设备 (COMx)」 | 红灯不亮先查 EN 与模组是否插在 **H3** |
| 打开端口 | 若上位机拉 DTR/RTS，板子可能**复位一次**（pitfalls D3），1~2 s 内恢复 | 上位机应重试，不要用「一打开必须立刻有数据」做判据 |
| 收流开始（**本 demo 看"行"**） | 每秒约 **5 行**以 `GESTURE: target=` 开头的文本，字段齐全；**没有任何中文/ASCII 日志文本** | 行率 5±20% 行/秒；坏行率 < 1%（由测试工程师的流校验工具统计） |
| 混在流里的二进制帧 | **偶发**出现以 `45 44` 开头、长度 11~4096、累加校验正确的点云帧 —— 这是**正常数据，必须原样到达** | 帧校验通过率应 100%；若点云帧残缺 ⇒ 查 `usb_tx_dropped` |
| 挥手 | 在手前方 0.2~0.8 m 处左/右/上/下挥手，`action` 在 -2~2 之间变化，x/y/z 跟随位置 | 与厂商界面对照（同一模组插上位机直连的结果） |
| 全程（诊断通道） | `uart_rx_overflows = 0`、`usb_tx_dropped = 0`、**`uart_frame_errs = 0`** | 打开 `APP_EDK60_EDV163_GESTURE_UART0_DEBUG=y`，USB-TTL 接 H2 的 D0/D1（115200）看统计行；同时 USB 数据流里**不应**出现这些统计文字 |

**60 秒什么都没有？按这个顺序查（覆盖本项目 95% 的现场问题）**：

1. 红灯亮不亮（EN=IO14 / 模组是否上电）；
2. 模组是否插在 **H3**（H4 的 T 与 EN 同网，插错会电源/数据互扰）；
3. 端口是不是那个「USB 串行设备」（板上没有 CH340/CP2102）；
4. **波特率是不是 115200**（不是 256000、也不是 921600）—— 错了的典型表现是 0 字节 +
   `uart_frame_errs` 上涨；
5. 打开 UART0 诊断通道看 `radar->pc` 计数是否为 0。

---

## 八、已知限制（与 5.2/5.3 同源，未新增）

| # | 限制 | 影响 | 处置 |
|---|---|---|---|
| 1 | 复位瞬间可能落几行 IDF 启动日志到 USB | 上位机看到一次「尾巴」，表现为少量坏行 | 横幅/引脚表已关，不含 `0x45 0x44` 相邻；上位机的切流规则会把它当认不出的行丢掉 |
| 2 | 打开串口可能触发板子复位（DTR/RTS 硬件行为） | 刚开始 1~2 s 无数据 | 上位机重试 |
| 3 | 上位机若长时间（>60 ms）不读 USB，本批会丢字节**但计数** | 偶发丢行/点云帧残缺 | 用 `usb_tx_dropped` 判定；上位机保持连续读 |
| 4 | 雷达 EN 不做运行期控制 | 无法软件复位雷达 | 有意为之（历史坑） |
| 5 | 不切行、不校验、不重传、不缓存断链期间数据、**不过滤二进制帧** | 语义就是「透明管道」 | 由上位机的切流/解析层负责 |
| 6 | 未做 USB 拔出/插入检测 | 拔线后写失败进入退避循环（计数可见） | 插回后自动恢复 |
| 7 | 固件不解析 `GESTURE:`、不映射 `action`、不算距离触发 | 全部在上位机 | 设计如此（桥的纪律） |
| 8 | USB→UART 通路保留但本代模组无命令接口 | 常态空闲，占 4096 B RX 缓冲 | 有意保留：模组将来若提供命令，桥无需改固件 |

---

## 九、留给下一步

| # | 事项 | 责任 |
|---|---|---|
| 1 | 执行 6.6 的三条命令，回填体积/回归（**19/19**）/SHA256 | 项目经理（提权环境） |
| 2 | 流校验工具 `tools/edv163_gesture_check.py`：统计行率/坏行率/二进制帧跳过率，**含「两行文本中间夹一帧二进制」用例** | 测试工程师 |
| 3 | 上板 10 分钟：行率 5±20%、坏行率 < 1%、点云帧校验通过率 100%、`uart_frame_errs == 0`、`uart_rx_overflows == 0`、无 >1 s 空档 | 测试工程师 + 项目经理 |
| 4 | 决定 `%TEMP%` 里那三个验证脚本是否提升为 `tools/syntax_check_edv163_gesture.ps1`（我倾向保留：改配置/加断言时可一键回归） | 项目经理 |
| 5 | 网页端 `release/gesture.html` 的切流规则必须与厂商实现逐条对齐（尤其「校验失败只丢 1 字节」），否则会吞掉后续文本行 | 全栈工程师 |
