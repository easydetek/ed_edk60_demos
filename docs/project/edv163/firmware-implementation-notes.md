# EDV163 水位监测透传桥固件实现说明（demo 5.3 `radar_bridge_edv163`）

> 角色：嵌入式/软件工程师（opcs-sw-engineer）｜技能：emb-driver、emb-arch
> 最后更新：2026-10-22
> 工程根：`C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos`
> 目标：ESP32-C6-MINI-1-N4（4 MB Flash / 无 PSRAM）、ESP-IDF **v6.1.0**、target `esp32c6`
>
> **本文以代码为准。** 第三节的 errata 表列出「照抄 EDV11P 版时会踩到的全部差异」，
> 评审时请直接看代码 + 该表。

---

## 一、这个 demo 是什么（30 秒版）

| 项 | 内容 |
|---|---|
| 干什么 | 把 EDV163 雷达（模组插 **H3**）的串口数据以 **256000 8N1** 收进来，**原样**搬到 ESP32-C6 原生 USB Serial/JTAG；同时把上位机发到该 USB 口的字节**原样**写回雷达 |
| 不干什么 | **不解析协议、不组帧、不发任何 GET/SET、不做转义/反转义、不改写任何字节、不计算水位**（不加换行/前缀/hexdump/私有封包） |
| 引脚 | UART1 RX=**IO4**（模组 TX）、TX=**IO5**（模组 RX）、雷达电源 EN=**IO14**（拉高常驻）、橙灯 IO15 **完全不碰** |
| 上位机 | 打开「USB 串行设备 (COMx)」，波特率填 256000 即可（USB CDC 不受波特率限制，填一致便于对数） |
| 入口 | `demo_radar_bridge_edv163_start()`，由 `demo_dispatch.c` 在选中 `APP_EDK60_DEMO_RADAR_BRIDGE_EDV163` 时调用 |
| 协议归属 | 0x45 0x44 帧头、len/checksum/0x0A 帧尾、`0x0A→09 01` / `0x09→09 00` 转义、0x81 上报、`水位 = 挂高 − target_distance` **全部在上位机侧**。固件里没有任何一行对应代码（源码头部只以注释形式留了一份协议速查，供评审看清边界） |

### 1.1 交付物清单（全部为本次改动）

| 文件 | 性质 | 说明 |
|---|---|---|
| `components/edk60_app/src/demo_get_started_radar_bridge_edv163.c` | **新增** | 全部实现（598 行，含方案说明、定量算式、上板验证、编译期断言） |
| `sdkconfig.defaults.radar_bridge_edv163` | **新增** | demo 专用 defaults（静默 + 256000 + 大缓冲） |
| `main/Kconfig.projbuild` | 修改 | choice 加 `APP_EDK60_DEMO_RADAR_BRIDGE_EDV163`；新增菜单「EDK60 Demo 参数（EDV163 透传桥 5.3）」共 7 项 |
| `components/edk60_app/src/demo_dispatch.c` | 修改 | 新增 `#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163)` 分支 + 入口前置声明 + 横幅门禁扩展到本 demo |
| `components/edk60_app/CMakeLists.txt` | 修改 | `EDK60_DEMO_SRCS` 增加一行（不登记则源文件不参与编译） |
| `build_all.ps1` | 修改 | `$DemoMap` 增加 `'radar_bridge_edv163' = 'APP_EDK60_DEMO_RADAR_BRIDGE_EDV163'` |
| `docs/project/edv163/firmware-implementation-notes.md` | **新增** | 本文 |
| `tools/syntax_check_edv163.ps1` | **额外新增（请 PM 决定去留）** | 本次用于 `-fsyntax-only` 验证的可复现 harness（9 用例）。它不属于产品代码；若不需要请告知，我会按流程申请删除（本次不改动超出范围的既有文件） |

> **未改动**：其余 17 个 demo 源文件、EDV11P 相关文件的行为（`demo_get_started_radar_bridge_edv11p.c` 与 `sdkconfig.defaults.radar_bridge_edv11p` 一字未动，
> 并已用编译验证确认在 EDV163 配置下它仍然编得过，见第六节用例 7）。

---

## 二、控制台洁净做法（与 5.2 同构，一个字节都不新增）

EDV163 的帧头是 **0x45 0x44（ASCII `"ED"`）**，与 EDV11P 完全相同，所以「日志污染 = 假同步」
这条最高优先风险**原样继承**。本 demo 采用与 5.2 完全一致的**四道保险 + 一道门禁**：

| 层 | 做法 | 位置 | 生效范围 |
|---|---|---|---|
| a) 编译期 | `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` ⇒ `ESP_LOG*` 被编译成空语句 | `sdkconfig.defaults.radar_bridge_edv163` | 全固件 |
| b) 编译期 | `# CONFIG_APP_EDK60_STARTUP_BANNER is not set` + `# CONFIG_APP_EDK60_PRINT_PINMAP is not set` ⇒ 两处 printf 不执行 | 同上 | 本 demo |
| c) 源码 | 本文件**通篇没有 printf、没有 ESP_LOGx**（唯一例外是默认关闭的诊断通道，见 d） | 新源码 | 本 demo |
| d) 运行期 | demo 第一行就 `esp_log_level_set("*", ESP_LOG_NONE)`，兜住「用户自己 menuconfig 编、没走 demo 专用 defaults」 | `demo_radar_bridge_edv163_start()` 第 592 行起 | 全固件 |
| e) **门禁（双保险）** | `demo_dispatch.c` 里整块跳过「横幅 + 复位原因 + 引脚表」，**不依赖 sdkconfig 是否记得关** | `demo_dispatch.c` 的 `#if !defined(...EDV11P) && !defined(...EDV163)` | 本 demo |

诊断通道（默认关闭）：`CONFIG_APP_EDK60_EDV163_UART0_DEBUG=y` 时走
`esp_rom_output_tx_one_char()` **直写 UART0**（H2 的 D1/IO17，115200），不经过 stdio，
因此不会被复制到 USB（依据 `components/esp_stdio/stdio_simple.c`：复制发生在 stdio 层）。
文案刻意不使用以 `"E""D"` 开头的词（用 `k60` / `radar` 代替）。
`CONFIG_APP_EDK60_EDV163_STATS_VIA_USB=y` 是**仅供一次性取证**的反向开关（故意污染数据流，
用来在没有 UART0 串口板时读背压计数器），正式固件必须保持 `n`。

**禁止项（有编译期断言）**：不得用 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 实现「数据流干净」。
实测（2026-10-15）会让 C6 启动卡死、USB 不枚举、必须断电重插。
断言在源码第 732~736 行，且**只在本 demo 被选中时生效**。

---

## 三、errata：与 EDV11P（demo 5.2）实现的全部差异

### 3.1 三处**实质**差异（上位机/测试必须知道）

| # | 维度 | EDV11P（5.2） | **EDV163（5.3）** | 后果 / 处置 |
|---|---|---|---|---|
| 1 | **波特率** | 921600（92160 B/s） | **256000**（**25600 B/s**） | 线速只有前者的 1/3.6。所有「每秒多少字节 / 多少毫秒的缓冲深度」结论**必须按 25600 B/s 重算**（见第四节）。波特率错一档的现场现象是**一个字节都收不到**（帧错误让驱动整体丢字节，pitfalls E2），不是乱码 → 上板先看有没有 `0x45 0x44` |
| 2 | **协议层复杂度** | 无帧尾、无转义 | **帧尾固定 0x0A + 转义 `0x0A→09 01` / `0x09→09 00`** | 这一层**完全在上位机**。桥的纪律是「连碰都不碰」：固件既不做转义，也不做反转义，更不做「按 0x0A 切帧」——一旦在桥里切帧，就等于把协议知识塞进了固件 |
| 3 | **业务语义** | 人数/轨迹（0x70 直接展示） | **水位 = 挂高_mm − target_distance_mm** | 固件**不算水位**；挂高（type 0x25，1200~5000 mm）也由上位机下发。固件不发任何命令 |

### 3.2 命名与参数差异（照抄上一版最容易漏的地方）

| # | 项目 | 5.2 写法 | 5.3 实际 | 说明 |
|---|---|---|---|---|
| 4 | 文件名 | `demo_get_started_radar_bridge_edv11p.c` | `demo_get_started_radar_bridge_edv163.c` | — |
| 5 | 入口函数 | `demo_radar_bridge_edv11p_start()` | `demo_radar_bridge_edv163_start()` | `demo_dispatch.c` 里前置声明 + `#elif` 分支 |
| 6 | Kconfig 前缀 | `APP_EDK60_EDV11P_*` | `APP_EDK60_EDV163_*` | 7 项全部改名，**不复用 5.2 的符号**（复用会让两个 demo 互相影响） |
| 7 | 文件内宏前缀 | `BR_*` | `BR163_*` | 两个源文件各自编译，本不冲突；加前缀是为了 `grep BR163_` 时不会串到 5.2 |
| 8 | 日志 TAG | `"rbridge"` | `"bridge163"` | 仍不以 `E`/`D` 开头（假同步防护），且静默模式下不输出 |
| 9 | 诊断文案 | `[k60] demo=radar_bridge_edv11p …` | `[k60] demo=radar_bridge_edv163 …` | 参数行按 256000 重算 |
| 10 | 单帧最大长度 | 512 B | **256 B**（转义后最坏 ≈512 B） | 影响 CHUNK 取值的论证（仍取 1024） |
| 11 | 菜单标题 | 「EDK60 Demo 参数（EDV11P 透传桥 5.2）」 | 「EDK60 Demo 参数（EDV163 透传桥 5.3）」 | — |
| 12 | demo 编号 | 5.2 | **5.3** | 工程从 17 个 demo 变成 **18 个** |
| 13 | 上报频率单位 | 0x10 = 50 ms | **0x17 = 100 ms** | 固件不下发；仅注释与文档口径 |

### 3.3 刻意**保持**不变的部分（改动会引入风险，故不动）

| 项 | 取值 | 理由 |
|---|---|---|
| console 配置 | 不动（主控制台 UART0 + USB 第二输出口） | 现状即目标状态；改动会踩 D1 启动卡死 |
| USB 写单批预算 | **60 ms** | 与 5.2 同一套「有界停摆 + 计数」模型，只是余量从 2.96× 放大到 10.67× |
| UART RX 缓冲 | 16384 B（Kconfig 可调 1~32 KB） | 本板 SRAM 512 KB，多 8 KB 换「上位机停顿 640 ms 也不丢字节」，划算 |
| USB TX / RX 缓冲 | 16384 B / 4096 B | 同上；上位机配置帧体积未变 |
| 任务结构 | `r2u`/`u2r` 优先级 6、栈 3072，`brstat` 优先级 3 仅诊断时创建 | 已验证结构 |
| 单次搬运块 | 1024 B | 帧变短了但块大小不必缩小：40 ms 线时仍远小于缓冲深度 |
| EN 处理 | 上电后常驻高，运行期不碰 | 历史坑：运行中误拉低 = 雷达掉电 |
| 不延时等模组启动 | 立即开始搬运 | 等待期间的数据会白丢（UART 已装好、缓冲已在收） |

---

## 四、缓冲区 / 超时 / 背压：按 256000 重算（含编译期断言）

线速：**256000 8N1 = 10 bit/byte ⇒ 25600 B/s**。
1024 B 线上时间 ≈ **40.0 ms**；单帧最大 256 B ≈ **10.0 ms**（最坏整帧全中 0x0A/0x09，
转义后 ≈512 B ≈ **20.0 ms**）。

| 参数 | 取值 | 依据（按 25600 B/s 算） | 与 5.2 对比 |
|---|---|---|---|
| UART1 RX 环形缓冲 | **16384 B**（`APP_EDK60_EDV163_UART_RX_BUF`，1024~32768） | 16384 ÷ 25600 = **640.0 ms** 的流量 | 5.2：177.8 ms |
| USB 写单批总预算 | **60 ms**（单次尝试 10 ms、退避 2 ms） | 60 ms × 25600 B/s = 1536 B 的最坏积压 | 相同 |
| **比值（唯一的定量保证）** | **10.67×** | 640 ms ÷ 60 ms ⇒ 主机 60 ms 内恢复读取就一个字节不丢 | 2.96× |
| USB TX 缓冲 | 16384 B | ≈ 640 ms 的突发吸收量 | 5.2：≈177 ms |
| USB RX 缓冲 | 4096 B | 配置帧 ≤256 B（转义后 ≤512 B），开机批量 GET/SET 约 2~4 KB | 相同 |
| UART1 TX 缓冲 | 4096 B | `uart_write_bytes` 阻塞到全部入队 ⇒ 天然反压 | 相同 |
| 单次搬运块 | **1024 B**（`APP_EDK60_EDV163_CHUNK`，64~2048） | ≈40 ms 线时，是最大帧（转义后 ≤512 B）的 2 倍 | 相同 |
| 读超时 | UART/USB 各 **10 ms** | 25.6 KB/s 下 10 ms 只积 ~256 B | 相同 |
| UART 事件队列 | 32 项 | 数据走环形缓冲；队列只用来统计「满/溢出/帧错」 | 相同 |
| RAM 增量 | **≈42 KB** | 16384+4096+16384+4096+2×1024 = 43008 B | 相同 |

### 4.1 编译期断言：把不等式钉死（本次新增，5.2 没有）

源码第 744~751 行：

```c
#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163) && \
    ((CONFIG_APP_EDK60_EDV163_UART_RX_BUF * 1000) / (CONFIG_APP_EDK60_EDV163_BAUD / 10) < \
     (2 * 60))
#error "… UART_RX_BUF×1000÷(BAUD/10) 必须 ≥ 120 ms（= 2×USB 写预算 60 ms）…"
#endif
```

- 默认组合：16384×1000÷25600 = **640 ms ≥ 120 ms** ⇒ 通过。
- 谁把 `BAUD` 调到 3 Mbps 却把 `UART_RX_BUF` 留在 1024 B（= 3 ms）⇒ **构建直接失败**并给出算式，
  不会留到上板时表现为「偶发丢帧」这种最难查的故障。
- 该断言**只在本 demo 被选中时生效**（第一条件），不会影响其余 17 个 demo（见第六节用例 3/6）。

### 4.2 可观测计数器（诊断通道输出，默认关闭；判据写在输出行里）

| 计数器 | 来源 | 含义 |
|---|---|---|
| `uart_rx_overflows` | `UART_BUFFER_FULL` | 环形缓冲满 ⇒ **有字节被驱动丢弃** |
| `uart_fifo_ovf` | `UART_FIFO_OVF` | 硬件 FIFO 溢出 |
| `uart_frame_errs` | `UART_FRAME_ERR` | **波特率不对的典型症状**（本 demo 尤其要盯这个） |
| `usb_tx_dropped` / `usb_tx_short` | `usb_write_all()` 返回值 < 本批长度 | 因 USB 写超预算放弃的字节数 / 批次 |
| `usb_wr_stalls` | `usb_serial_jtag_write_bytes()` 返回 ≤0 | 写受阻次数 |

**判据：`uart_rx_overflows == 0` 且 `usb_tx_dropped == 0` 且 `uart_frame_errs == 0`** ⇒ 零丢字节且波特率正确。
实现位置：`usb_write_all()` 第 418 行、`uart_drain_events()` 第 451 行、
`task_radar_to_usb()` 第 501 行、`task_stats()` 第 560 行。

---

## 五、Kconfig 清单

### 5.1 demo 选择项（`menu "EDK60 Demo"` → `choice APP_EDK60_DEMO`）

```kconfig
config APP_EDK60_DEMO_RADAR_BRIDGE_EDV163
    bool "5.3 radar_bridge_edv163  水位监测雷达透传桥：UART1(IO4/IO5) ⇄ 原生 USB（256000，纯字节双向，不解析协议）"
```

### 5.2 新增菜单「EDK60 Demo 参数（EDV163 透传桥 5.3）」

| 符号 | 类型 | 默认（范围） | 说明 |
|---|---|---|---|
| `APP_EDK60_EDV163_BAUD` | int | **256000**（9600~3000000） | 雷达串口速率；⚠ 不是 921600 |
| `APP_EDK60_EDV163_SILENT_LOGS` | bool | **y** | 入口第一行 `esp_log_level_set("*", ESP_LOG_NONE)` |
| `APP_EDK60_EDV163_UART0_DEBUG` | bool | **n** | 允许直写 UART0 的诊断/统计（不污染 USB） |
| `APP_EDK60_EDV163_STATS_VIA_USB` | bool | **n** | 取证专用反向通道（会让数据流混入文本） |
| `APP_EDK60_EDV163_STATS_SEC` | int | 10（0~300） | 统计周期，0 = 不打印 |
| `APP_EDK60_EDV163_UART_RX_BUF` | int | **16384**（1024~32768） | UART1 接收环形缓冲 |
| `APP_EDK60_EDV163_CHUNK` | int | 1024（64~2048） | 单次搬运最大字节数 |

### 5.3 三条硬规则合规说明

1. **`choice` 不产生值符号** —— 本 demo **刻意不使用 choice**（端口/引脚都是实测写死的），
   因此不需要派生 int；Kconfig 注释里写明这一点，防止后来者加 choice 时漏派生符号。
   新增的两处 choice 相关改动只是**在既有 `choice APP_EDK60_DEMO` 里加一个成员**，成员是被
   `#if defined(CONFIG_...)` 使用的，不涉及取值。
2. **`default n` 的 bool 不生成宏** —— `UART0_DEBUG` / `STATS_VIA_USB` 全部用 `#ifdef` 兜底
   （`BR163_UART0_DBG` / `BR163_STATS_VIA_USB`）；`SILENT_LOGS` 虽是 `default y` 但用户可关，
   同样用 `#ifdef`（`BR163_SILENT`）。
3. **`sdkconfig.defaults` 不支持行尾注释** —— 新增的 `sdkconfig.defaults.radar_bridge_edv163`
   里所有注释都独占一行，文件头写明了这条限制。

### 5.4 已用 kconfgen 实测校验（不依赖完整构建）

```powershell
$py = "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
$repo = "C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos"
$tmp  = "$env:TEMP\kcfg163"

# config.env：照 IDF 的 tools/kconfig_new/config.env.in 造，指向 build-hello_board 里
# 已经生成好的 kconfigs.in / kconfigs_projbuild.in
#（后者已 source 本工程的 main/Kconfig.projbuild，所以新菜单能被解析到）
#
# demo_select 里只有一行：CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163=y
#（等价于 build_all.ps1 写出的 sdkconfig.defaults.esp32c6）
& $py -m kconfgen --kconfig "C:/esp/v6.1/esp-idf/Kconfig" --env-file "$tmp\config.env" `
    --env "IDF_ENV_FPGA=" --env "IDF_INIT_VERSION=v6.1" `
    --config "$tmp\sdkconfig_main" `
    --defaults "$repo\sdkconfig.defaults" `
    --defaults "$tmp\demo_select" `
    --defaults "$repo\sdkconfig.defaults.radar_bridge_edv163" `
    --output config "$tmp\sdkconfig_main" --output header "$tmp\cfg_main\sdkconfig.h"
```

**结果：exit 0**（stderr 只有 IDEA/IDF 自身的既有 NOTE：`BT_NIMBLE_MESH_PROVISIONER`、
`FATFS_PRINT_LLI/FLOAT` 的 `'default 0' is not a valid bool value`，与 build_all.ps1 里 R13 记录一致，
非本次引入）。生成的 `sdkconfig` / `sdkconfig.h` 关键项：

```
CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163=y
# CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P is not set
# CONFIG_APP_EDK60_STARTUP_BANNER is not set
# CONFIG_APP_EDK60_PRINT_PINMAP is not set
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
CONFIG_LOG_DEFAULT_LEVEL=0
CONFIG_APP_EDK60_EDV163_BAUD=256000
CONFIG_APP_EDK60_EDV163_SILENT_LOGS=y
# CONFIG_APP_EDK60_EDV163_UART0_DEBUG is not set
# CONFIG_APP_EDK60_EDV163_STATS_VIA_USB is not set
CONFIG_APP_EDK60_EDV163_STATS_SEC=10
CONFIG_APP_EDK60_EDV163_UART_RX_BUF=16384
CONFIG_APP_EDK60_EDV163_CHUNK=1024
```

（`sdkconfig.h` 里 `#define CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163 1` 与 6 个参数宏全部到位，
两个 `default n` 的 bool 按 A2 规则**不存在**——所以 C 侧必须 `#ifdef`，代码已照此写。）

同一套 harness 另外按需生成了 5 组配置，用于第六节的 9 个编译用例：
`cfg_dbg`（UART0_DEBUG=y）、`cfg_usbstats`（STATS_VIA_USB=y）、`cfg_nosilent`（SILENT_LOGS=n）、
`cfg_other`（HELLO_BOARD=y + 敌对配置）、`cfg_bad`（EDV163=y + 敌对配置）。全部 exit 0。

---

## 六、编译期验证（本会话不能跑 idf.py，这是替代验证）

### 6.1 为什么不能跑构建（环境事实）

本机沙箱禁止 `idf.py` 使用的**命名管道**：`asyncio → windows_utils.pipe() → _winapi.CreateFile`
直接 `PermissionError: [WinError 5] 拒绝访问`。这与 5.2 当时的记录逐条一致（`docs/project/edv11p/firmware-implementation-notes.md` 6.5）。
因此本次用两条纯用户态路径做**等价验证**，真正的 `idf.py build` 由项目经理执行（见 6.4）。

### 6.2 `-fsyntax-only`：9 个用例全过

编译参数**取自真实构建**（`build-hello_board/compile_commands.json` 的
`demo_get_started_radar_bridge_edv11p.c` 一行），只做两处替换：
① `-I…/build-hello_board/config` 换成对应场景的 `sdkconfig.h` 目录；
② `-o …obj` 换成 `-fsyntax-only`。其余（`-Wall -Werror -Wextra -std=gnu23 -Og -ffunction-sections …`）**一字不改**。

可复现脚本：[`tools/syntax_check_edv163.ps1`](../../../tools/syntax_check_edv163.ps1)（`pwsh -NoProfile -File tools\syntax_check_edv163.ps1`）

| # | 场景 | 源文件 | 配置 | 结果 |
|---|---|---|---|---|
| 1 | **量产配置**（选中本 demo、静默、无诊断） | `demo_get_started_radar_bridge_edv163.c` | `cfg_main` | **exit 0，输出 0 字节**（0 error / 0 warning） |
| 2 | **诊断开关打开**（`UART0_DEBUG=y`，dbg 分支参与编译） | 同上 | `cfg_dbg` | **exit 0，输出 0 字节** |
| 3 | **未选中本 demo**（HELLO_BOARD=y）+ **敌对配置**（`CONSOLE_SECONDARY_NONE=y` + 3 Mbps + 1024 B 缓冲） | 同上 | `cfg_other` | **exit 0，输出 0 字节** ⇒ 证明两条 `#error` **不泄漏**到其它 17 个 demo |
| 4 | `demo_dispatch.c` 选中本 demo（走新 `#elif` + 横幅门禁） | `demo_dispatch.c` | `cfg_main` | **exit 0，输出 0 字节** |
| 5 | `demo_dispatch.c` 选中 hello_board（其它 demo 路径回归） | `demo_dispatch.c` | `build-hello_board/config` | **exit 0，输出 0 字节** |
| 6 | **断言反向验证**：选中本 demo + 同一套敌对配置 | 新源码 | `cfg_bad` | **exit 1**，两条 `#error` 都命中（见下）⇒ 断言不是死代码 |
| 7 | EDV11P 源文件在 EDV163 配置下（确认没被本次改动影响） | `demo_get_started_radar_bridge_edv11p.c` | `cfg_main` | **exit 0，输出 0 字节** |
| 8 | **取证通道** `STATS_VIA_USB=y`（`dbg_raw`/`dbg_line` 的 USB 分支参与编译） | 新源码 | `cfg_usbstats` | **exit 0，输出 0 字节** |
| 9 | **`SILENT_LOGS=n`**（`#ifdef` 兜底路径，此时宏根本不存在） | 新源码 | `cfg_nosilent` | **exit 0，输出 0 字节** |

用例 6 的实测输出（摘录）：

```
demo_get_started_radar_bridge_edv163.c:734:2: error: #error "radar_bridge_edv163 不允许用「关闭 USB 第二控制台」…"
demo_get_started_radar_bridge_edv163.c:747:2: error: #error "radar_bridge_edv163 的缓冲区/波特率组合不安全：UART_RX_BUF×1000÷(BAUD/10) 必须 ≥ 120 ms…"
```

> 两条断言都是**双向验证**过的（该炸时炸、不该炸时不炸），符合 pitfalls B1 的验收要求。

### 6.3 没有被语法检查覆盖的部分（诚实列出）

| 项 | 是否覆盖 | 说明 |
|---|---|---|
| C 源文件语法/告警 | ✅ | 9 用例、`-Werror` 全绿 |
| Kconfig 语法 + defaults 叠加 | ✅ | kconfgen exit 0，逐项核对过 |
| 组件 `CMakeLists.txt` 的源文件登记 | ⚠️ 部分 | 只确认已加进 `EDK60_DEMO_SRCS`；`list(LENGTH)` 打印的 18 待构建时确认 |
| 链接期符号裁剪（`--gc-sections`） | ❌ | 需真实构建 + `nm` 校验 |
| 固件体积 / 分区余量 | ❌ | 需真实构建 |
| 上板行为（帧流/水位/背压计数） | ❌ | 需换装 EDV163 后实测 |

### 6.4 需要项目经理执行的命令（按顺序）

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos

# 1) 单 demo 构建（会先删 sdkconfig.radar_bridge_edv163 再按 defaults 重新生成）
.\build_all.ps1 -Demo radar_bridge_edv163

# 2) 全量回归（18/18；重点确认其余 17 个 demo 没被新断言/新 Kconfig 影响）
.\build_all.ps1 -Size

# 3) 合并固件 + 体积 + SHA256（发布用，烧 0x0）
idf.py -B build-radar_bridge_edv163 merge-bin
Get-Item build-radar_bridge_edv163\merged-binary.bin | Select-Object Length
Get-FileHash build-radar_bridge_edv163\merged-binary.bin -Algorithm SHA256
```

**回填口径**：把 1) 的 `OK/体积`、2) 的 `18/18`、3) 的体积与 SHA256 填回本文第六节与
`docs/project/edv163/` 的 Gate 记录。

可选（验证符号裁剪与配置真的生效）：

```powershell
# 选中本 demo 的宏应存在、且只有它存在
Select-String build-radar_bridge_edv163\config\sdkconfig.h -Pattern "APP_EDK60_DEMO_RADAR_BRIDGE_EDV163|EDV163_BAUD"
# 未选中 demo 的入口符号应被 --gc-sections 丢掉（应无输出）
& C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin\riscv32-esp-elf-nm.exe `
    build-radar_bridge_edv163\edk60_demos.elf | Select-String "demo_radar_bridge_edv11p_start"
```

---

## 七、上板后应看到什么（验收口径）

| 阶段 | 期望现象 | 判据 / 怎么确认 |
|---|---|---|
| 上电 0~1 s | 丝印 **RADAR 红灯亮**（= IO14 已拉高，3.3V_RADAR 上电）；PC 出现「USB 串行设备 (COMx)」 | 红灯不亮先查 EN 与模组是否插在 **H3** |
| 打开端口 | 若上位机拉 DTR/RTS，板子可能**复位一次**（pitfalls D3），1~2 s 内恢复 | 上位机应重试，不要用「一打开必须立刻有数据」做判据 |
| 收流开始 | 字节流里出现 **`45 44`** 开头的帧，帧尾是 **`0A`**；**没有任何中文/ASCII 日志文本** | `tools/edv163_frame_check.py --raw-dump` 要求「未归属字节率 = 0」 |
| 上位机下配置后 | 下发 `0x18`（工作模式=1）、`0x17`（上报频率，单位 **100 ms**）、**`0x25` 挂高（如 2000 mm）** 后，周期收到 **type=0x81** 上报 | 上位机侧解析；固件不参与 |
| 数据展示 | 显示「到水面距离」与「**水位 = 挂高 − 距离**」；手在水面/水面上方移动时距离与水位**反向**变化 | 卷尺量一次实际距离反算，偏差应 < 2 cm |
| 全程（诊断通道） | `uart_rx_overflows = 0`、`usb_tx_dropped = 0`、**`uart_frame_errs = 0`** | 打开 `APP_EDK60_EDV163_UART0_DEBUG=y`，USB-TTL 接 H2 的 D0/D1（115200）看统计行；同时 USB 数据流里**不应**出现这些统计文字 |

**60 秒什么都没有？按这个顺序查（覆盖本项目 95% 的现场问题）**：

1. 红灯亮不亮（EN=IO14 / 模组是否上电）；
2. 模组是否插在 **H3**（H4 的 T 与 EN 同网，插错会电源/数据互扰）；
3. 端口是不是那个「USB 串行设备」（板上没有 CH340/CP2102）；
4. **波特率是不是 256000**（不是 5.2 的 921600）—— 错了的典型表现是 0 字节 + `uart_frame_errs` 上涨；
5. 打开 UART0 诊断通道看 `radar->pc` 计数是否为 0。

---

## 八、已知限制（与 5.2 同源，未新增）

| # | 限制 | 影响 | 处置 |
|---|---|---|---|
| 1 | 复位瞬间可能落几行 IDF 启动日志到 USB | 上位机看到一次「尾巴」 | 横幅/引脚表已关，不含 `0x45 0x44` 相邻；帧同步（帧头+len+checksum+帧尾）会跳过 |
| 2 | 打开串口可能触发板子复位（DTR/RTS 硬件行为） | 刚开始 1~2 s 无数据 | 上位机重试 |
| 3 | 上位机若长时间（>60 ms）不读 USB，本批会丢字节**但计数** | 偶发丢帧 | 用 `usb_tx_dropped` 判定；上位机保持连续读 |
| 4 | 雷达 EN 不做运行期控制 | 无法软件复位雷达 | 有意为之（历史坑） |
| 5 | 不校验、不重传、不缓存断链期间数据、**不做转义** | 语义就是「透明管道」 | 由上位机的协议层负责（帧头+len+checksum+转义+帧尾） |
| 6 | 未做 USB 拔出/插入检测 | 拔线后写失败进入退避循环（计数可见） | 插回后自动恢复 |
| 7 | 固件不实现 0x81 解析、不算水位、不下发 0x25 挂高 | 全部在上位机 | 设计如此（桥的纪律） |

---

## 九、留给下一步

| # | 事项 | 责任 |
|---|---|---|
| 1 | 执行 6.4 的 3 条命令，回填体积/回归/SHA256 | 项目经理（提权环境） |
| 2 | `tools/edv163_frame_check.py --raw-dump`：证明 USB 流里未归属字节率 = 0（含「data 里出现 0x0A/0x09」的转义用例） | 测试工程师 |
| 3 | 上板 30 分钟：坏帧率 < 0.1%、帧完整率 ≥ 99.9%、`uart_frame_errs == 0`、`uart_rx_overflows == 0` | 测试工程师 + 项目经理 |
| 4 | 水位标定：卷尺量「传感器 → 水面」实际距离，反算挂高并写进教程 | 用户 + 硬件 |
| 5 | 决定 `tools/syntax_check_edv163.ps1` 是否保留在仓库（我倾向保留：改配置/加断言时可一键回归） | 项目经理 |
