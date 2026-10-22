# EDV11P 透传桥固件实现说明（demo 5.2 `radar_bridge_edv11p`）

> 角色：嵌入式/软件工程师 ｜ 任务：T1-1 / T1-2 / T1-3 ｜ 最后更新：2026-10-21
> 工程根：`C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos`
> 目标：ESP32-C6-MINI-1-N4（4MB Flash / 无 PSRAM）、ESP-IDF **v6.1.0**、target `esp32c6`
>
> **本文以代码为准。**第二节那张 errata 表列出了「方案文档建议 vs 实际代码」的全部差异，
> 评审时请直接看代码 + 该表，不要以早期方案文档为准。

---

## 一、这个 demo 是什么（30 秒版）

| 项 | 内容 |
|---|---|
| 干什么 | 把 EDV11P 雷达（模组插 **H3**）的串口数据以 **921600 8N1** 收进来，**原样**搬到 ESP32-C6 原生 USB Serial/JTAG；同时把上位机发到该 USB 口的字节**原样**写回雷达 |
| 不干什么 | **不解析协议、不组帧、不发任何 GET/SET、不改写任何字节**（不加换行/前缀/hexdump/私有封包）。上一代 EDV151 的开机六条握手在本 demo **不存在** |
| 引脚 | UART1 RX=**IO4**（模组 TX）、TX=**IO5**（模组 RX）、雷达电源 EN=**IO14**（拉高常驻）、橙灯 IO15 **完全不碰** |
| 上位机 | 打开「USB 串行设备 (COMx)」，波特率填 921600 即可（USB CDC 不受波特率限制，填一致便于对数） |
| 入口 | `demo_radar_bridge_edv11p_start()`，由 `demo_dispatch.c` 在选中 `APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P` 时调用 |

### 交付物清单

| 文件 | 性质 | 说明 |
|---|---|---|
| `components/edk60_app/src/demo_get_started_radar_bridge_edv11p.c` | 新增 | 全部实现（含方案说明与验证方法注释） |
| `main/Kconfig.projbuild` | 修改 | choice 加 `APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P`；新增菜单「EDK60 Demo 参数（EDV11P 透传桥 5.2）」共 6 项 |
| `components/edk60_app/src/demo_dispatch.c` | 修改 | 新增 `#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P)` 分支 + 入口前置声明 + 横幅门禁 |
| `components/edk60_app/CMakeLists.txt` | 修改 | `EDK60_DEMO_SRCS` 增加一行（不登记则源文件不参与编译） |
| `sdkconfig.defaults.radar_bridge_edv11p` | 新增 | demo 专用 defaults（build_all.ps1 自动拼接，后者覆盖前者） |
| `build_all.ps1` | 修改 | `$DemoMap` 增加 `'radar_bridge_edv11p' = 'APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P'` |
| `docs/project/edv11p/firmware-implementation-notes.md` | 新增 | 本文 |

> 未改动：其余 16 个 demo 源文件、`human_track_v1.4_side_mount.py`、`release/`、既有 docs。
> 证据见第八节的文件时间戳清单。

---

## 二、errata：方案文档 vs 实际代码（以代码为准）

| # | 早期方案/文档说法 | 代码实际 | 采用理由 |
|---|---|---|---|
| 1 | 「把 primary console 从 USB 挪到 UART0」作为首选方案 | **本工程主控制台本来就是 UART0**（H2 的 D0=IO16 / D1=IO17），USB 只是第二输出口；不需要任何 console 改动 | 现状即目标状态。再改一次只会引入「启动卡死」风险（见 errata 3） |
| 2 | `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 让 USB 只跑数据 | **禁止使用**；demo 源码里有编译期 `#error` 拦住它 | 2026-10-15 实测：启动卡死在 `sleep_gpio: Enable automatic switching...`，USB 不枚举，必须断电重插。根因：该选项与 `USJ_ENABLE_USB_SERIAL_JTAG` 联动，USB 外设时钟被关 |
| 3 | 只设 `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` 就够 | 不够：还要**关掉启动横幅与引脚表**（它们是 printf，实测抓到 1142 字节控制台文本），并且 demo 里不用 printf/ESP_LOG | 实测记录在 `sdkconfig.defaults.radar_bridge` 文件头 |
| 4 | `CONFIG_FREERTOS_HZ=1000` | **未落地**，保持 IDF 默认 100 Hz | 本 demo 的时序裕量靠「UART 环形缓冲 + USB 缓冲」，不靠 tick 精度；改 HZ 会影响全部 17 个 demo 的时序假设，收益为零 |
| 5 | `CONFIG_APP_EDK60_BRIDGE_EDV11P_PWR_READY_MS`（使能后等待 N ms） | **不存在**。EN 拉高后**立即**开始搬运，不等待 | 等待期间的数据会白丢：UART 驱动已安装、环形缓冲已在收集，不等待就一个字节不丢 |
| 6 | 诊断输出用 `uart_write_bytes(UART_NUM_0, ...)` | 改用 `esp_rom_output_tx_one_char()` 直写 UART0 | `uart_write_bytes` 要求 UART0 驱动已安装，而 console 不装驱动（走 ROM 直写）。为打印诊断去接管控制台 TX 不划算；ROM 单字节输出这一层不会被复制到 USB（依据 `components/esp_stdio/stdio_simple.c`） |
| 7 | USB 写「失败重试 20 次 × 105 ms ≈ 2.1 s」 | 改为**单批总预算 60 ms**，超预算即放弃剩余字节并计数 | 2.1 s 远超 UART 环形缓冲 177.8 ms 的深度 ⇒ 会**静默丢字节**（Gate-1 评审官指出）。60 ms 才与缓冲深度构成可验证的定量关系 |
| 8 | 缓冲区「余量 ≥ 8 倍」 | 该表述**算术不成立，已删除**；正确关系见第四节 | 原式把「单次 100 ms 阻塞」与「89 ms 缓冲」当作 8 倍余量，方向都说反了 |
| 9 | UART RX 缓冲 8192 B | **16384 B**（Kconfig 默认值也改成 16384） | 60 ms（USB 写预算）× 92160 B/s = 5530 B；16384 B ≈ 177.8 ms 才留出 2.96× 关系，8192 B（≈88.9 ms）不成立 |
| 10 | 复用 `APP_EDK60_RADAR_UART_BAUD`（默认 256000） | **新符号** `APP_EDK60_EDV11P_BAUD`，默认 **921600** | 改共享符号会改变 4.3 / 5.1 两个已有 demo 的行为（零改动要求） |
| 11 | 上报频率 0x10 的单位是 10 ms | 单位是 **50 ms**；演示默认下发 **2 = 100 ms** | 协议文档 + 示例；本固件不下发该参数（由上位机下发），但注释已按 50 ms 更正 |
| 12 | 「模组型号 EDV11P」 | 分支**未定**（EDV11P-N-02 人数统计/轨迹跟踪候选）；上板第一步用 `GET 0x02` 回读版本并保存原始字节 | 版本决定 0x70 目标上报是否可用；固件不参与，但验收时要记录 |
| 13 | 在 `edk60_demo.h` 里加函数声明 | 声明写在 `demo_dispatch.c` 里 | 本次改动范围限定为「新 demo + 分派器」，不动被 17 个 demo 共用的头文件（效果等价） |

---

## 三、控制台洁净方案：选了什么、为什么、怎么验证

### 3.1 问题

ESP32-C6 的 USB Serial/JTAG 只有**一个**外设（IO12=D− / IO13=D+），PC 上只呈现为**一个** USB 串行设备。
本工程把 UART0 作为主控制台、把 USB 作为**第二输出口**（`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`），
于是在 stdio 层每个字符都被**写两份**：一份去 UART0，一份去 USB。

后果有两层：

1. 上位机看到「数据夹日志」的混合流；
2. **更致命**：日志里的板名以两个 ASCII 字节 `"E"`(0x45) + `"D"`(0x44) 开头，
   正好等于雷达帧头 ⇒ 上位机会把日志当成一帧的开头，之后整段解析错位（假同步）。

IDF 源码依据（`components/esp_stdio/stdio_simple.c`，v6.1.0）：

```c
void esp_system_console_put_char(char c)
{
    esp_rom_output_tx_one_char(c);                       /* 只写主控制台 = UART0 */
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG && !ESP_ROM_CONSOLE_OUTPUT_SECONDARY
    esp_rom_usb_serial_putc(c);                          /* 再复制一份到 USB */
#endif
}
```

### 3.2 三个候选与结论

| 候选 | 结论 | 依据 |
|---|---|---|
| ① 把主控制台挪到 UART0 | **不需要做**（现状即是） | `usb_serial_bridge.c` 头部注释「控制台（UART0，115200-8-N-1，不受本 demo 影响）」；`docs/EDK60-pinmap.md` 第五节：UART0 引到 H2 的 D0/D1，与 IO4/IO5/IO14 无冲突 |
| ② 关掉 USB 第二控制台 | **禁止** | 实测启动卡死 + USB 不枚举（`sdkconfig.defaults.radar_bridge` 文件头、`docs/EDK60-pitfalls.md` D1）。源码末尾有 `#error` 拦这一条 |
| ③ **保持 console 配置不动 + 不产生字符** | ✅ **采用** | 见 3.3 |

### 3.3 采用方案（四道保险）

| 层 | 做法 | 生效范围 |
|---|---|---|
| a) 编译期 | `sdkconfig.defaults.radar_bridge_edv11p` 里 `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` ⇒ `ESP_LOG*` 被编译成空语句 | 全固件 |
| b) 编译期 | 同文件里 `# CONFIG_APP_EDK60_STARTUP_BANNER is not set` + `# CONFIG_APP_EDK60_PRINT_PINMAP is not set` ⇒ 两处 printf 不执行；即使有人改回去，`demo_dispatch.c` 里还有 `#if !defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P)` 门禁**直接跳过整块横幅**（双保险，不依赖配置是否记得关） | 本 demo |
| c) 源码 | 本 demo **通篇没有 printf、没有 ESP_LOGx** | 本 demo |
| d) 运行期 | demo 第一行就 `esp_log_level_set("*", ESP_LOG_NONE)` ⇒ 兜住「用户自己 menuconfig 编、没走 demo 专用 defaults」的情况，顺带压住驱动层运行期日志 | 全固件 |

需要诊断时用 `CONFIG_APP_EDK60_EDV11P_UART0_DEBUG=y`（**默认关闭**）：
走 `esp_rom_output_tx_one_char()` 直写 UART0，**不经过 stdio，因此不会被复制到 USB**；
文案也刻意不使用以 "E""D" 开头的词（用 `k60` / `radar` 代替）。
统计任务只在该开关打开时创建（关闭时连任务都不占 RAM/CPU）。

### 3.4 怎么验证（三条，全部可复现）

1. **配置验证**：`Select-String -Path sdkconfig.defaults.radar_bridge_edv11p -Pattern "LOG_DEFAULT_LEVEL_NONE|STARTUP_BANNER|PRINT_PINMAP"` 三条全中；
   `menuconfig → EDK60 Demo → 5.2` 选中后，`sdkconfig` 里应出现
   `CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P=y`、`# CONFIG_APP_EDK60_STARTUP_BANNER is not set`、`CONFIG_LOG_DEFAULT_LEVEL_NONE=y`。
2. **运行验证**：PC 用 `tools/edv11p_frame_check.py --raw-dump`（测试工程师负责）抓 30 秒，
   要求「无法解析的字节数 = 0」——即流里除了合法帧没有多余字节。
3. **反向验证（必须做）**：把 `APP_EDK60_EDV11P_UART0_DEBUG` 打开，用 USB-TTL 接 H2 的 D0/D1（115200）
   应能看到统计行；**同时**USB 数据流里仍然看不到这些统计文字 ⇒ 证明诊断通道确实没走 USB。

> 已知残留：**上电/复位瞬间**（demo 第一行代码之前的 IDF 启动日志）仍可能落几行到 USB。
> 端口打开时 DTR/RTS 常触发一次复位（pitfalls D3），所以上位机可能看到一次启动尾巴。
> 这些字节里不含 `0x45 0x44` 相邻（横幅已关），帧同步（帧头 + 长度 + 校验和）会直接跳过。

---

## 四、缓冲区 / 任务 / 超时：算清楚

线速：921600 8N1 = 10 bit/byte ⇒ **92160 B/s**；1024 B 线上时间 ≈ 11.1 ms；单帧最大 512 B ≈ 5.6 ms。

| 参数 | 取值 | 依据 |
|---|---|---|
| UART1 RX 环形缓冲 | **16384 B**（Kconfig `APP_EDK60_EDV11P_UART_RX_BUF`，1~32 KB 可调） | 16384 ÷ 92160 ≈ **177.8 ms** 的流量 |
| USB 写单批总预算 | **60 ms**（`BR_USB_WR_BUDGET_MS`，单次尝试 10 ms、退避 2 ms） | 60 ms ≪ 177.8 ms ⇒ 比值 **2.96×**：主机 60 ms 内恢复读取就一个字节不丢 |
| USB TX 缓冲 | **16384 B** | ≈ 177 ms 的突发吸收量，上位机线程被调度走 100 ms 不会触发丢字节 |
| USB RX 缓冲 | **4096 B** | 配置帧很小（≤512 B），开机批量 GET/SET 约 2~4 KB；USB 侧是事务传输，设备缓冲满时主机会重发而非丢包 |
| UART1 TX 缓冲 | **4096 B** | `uart_write_bytes` 阻塞到全部入队（驱动内部 `portMAX_DELAY`），天然反压上位机 |
| 单次搬运块 | **1024 B**（Kconfig `APP_EDK60_EDV11P_CHUNK`） | 约为最大帧的 2 倍，减少循环次数又不长时间占住缓冲 |
| 读超时 | UART/USB 各 **10 ms** | 90 KB/s 下 10 ms 只积 ~900 B，远小于缓冲深度；兼顾响应与 CPU |
| UART 事件队列 | **32 项** | 数据走环形缓冲；事件队列用来统计「满了/溢出/帧错」 |

> ⚠ 参数之间是**耦合**的：改 `BR_USB_WR_BUDGET_MS` 或 `UART_RX_BUF` 必须重算上面这个 2.96× 关系，
> 源码里两处注释（文件头 + 宏定义处）都写了这条约束。

### 4.1 可观测计数器（P1-04，Gate-1 放行条件）

| 计数器 | 来源 | 含义 |
|---|---|---|
| `uart_rx_overflows` | UART 事件 `UART_BUFFER_FULL` | 环形缓冲满 ⇒ **有字节被驱动丢弃** |
| `uart_fifo_ovf` | UART 事件 `UART_FIFO_OVF` | 硬件 FIFO 溢出 ⇒ 字节在 ISR 之前就丢了 |
| `uart_frame_errs` | UART 事件 `UART_FRAME_ERR` | 帧错误（波特率不符/干扰的典型症状） |
| `usb_tx_dropped` | `usb_write_all()` 返回值 < 本批长度 | 因 USB 写超预算放弃的**字节数** |
| `usb_tx_short` | 同上 | 发生「短写」的**批次**数 |
| `usb_wr_stalls` | `usb_serial_jtag_write_bytes()` 返回 ≤0 | USB 写受阻（缓冲满/驱动异常）次数 |

**判据：`uart_rx_overflows == 0` 且 `usb_tx_dropped == 0` 才叫「零丢字节」。**
输出方式：`APP_EDK60_EDV11P_UART0_DEBUG=y` 时每 `APP_EDK60_EDV11P_STATS_SEC`（默认 10 s）一行，直写 UART0。
实现位置（`demo_get_started_radar_bridge_edv11p.c`）：`usb_write_all()` 第 338 行、
`uart_drain_events()` 第 371 行、`task_radar_to_usb()` 第 419 行、`task_stats()` 第 477 行。

### 4.2 任务与内存

| 任务 | 优先级 | 栈 | 触发 | 说明 |
|---|---|---|---|---|
| `r2u`（雷达→USB） | 6 | 3072 | 阻塞读 UART，10 ms 超时 | **唯一**写 USB 的任务 ⇒ 不需要锁，字节流不会撕裂 |
| `u2r`（USB→UART） | 6 | 3072 | 阻塞读 USB，10 ms 超时 | 把上位机的 GET/SET 原样下发 |
| `brstat`（统计） | 3 | 3072 | 每 N 秒 | **仅在 UART0_DEBUG 打开时创建** |

收发缓冲为静态数组（`s_buf_r2u` / `s_buf_u2r`，各 1024 B），不进栈；统计用 `portMUX` 临界区保护。
RAM 增量估算：16384（UART RX）+ 4096（UART TX）+ 16384（USB TX）+ 4096（USB RX）+ 2×1024（搬运缓冲）≈ **43 KB**。

---

## 五、Kconfig 清单

### 5.1 demo 选择项（`menu "EDK60 Demo"` → `choice APP_EDK60_DEMO`）

```kconfig
config APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P
    bool "5.2 radar_bridge_edv11p  跟踪/人数统计雷达透传桥：UART1(IO4/IO5) ⇄ 原生 USB（921600，纯字节双向，不解析协议）"
```

### 5.2 新增菜单「EDK60 Demo 参数（EDV11P 透传桥 5.2）」

| 符号 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `APP_EDK60_EDV11P_BAUD` | int | **921600**（9600~3000000） | 雷达串口速率 |
| `APP_EDK60_EDV11P_SILENT_LOGS` | bool | **y** | 入口第一行 `esp_log_level_set("*", ESP_LOG_NONE)` |
| `APP_EDK60_EDV11P_UART0_DEBUG` | bool | **n** | 允许直写 UART0 的诊断/统计（不污染 USB） |
| `APP_EDK60_EDV11P_STATS_SEC` | int | 10（0~300） | 统计周期，0 = 不打印 |
| `APP_EDK60_EDV11P_UART_RX_BUF` | int | **16384**（1024~32768） | UART1 接收环形缓冲 |
| `APP_EDK60_EDV11P_CHUNK` | int | 1024（64~2048） | 单次搬运最大字节数 |

### 5.3 三条硬规则合规说明

1. **choice 不产生值符号**：本 demo **刻意不使用 choice**（端口/引脚都是实测写死的），因此不需要派生 int；Kconfig 注释里写明了这一点，避免后来者加 choice 时漏派生符号。
2. **`default n` 的 bool 不生成宏**：`UART0_DEBUG` 全部用 `#ifdef` 兜底（`BR_UART0_DBG`）；`SILENT_LOGS` 虽是 `default y` 但用户可关，同样用 `#ifdef`（`BR_SILENT`）。
3. **`sdkconfig.defaults` 不支持行尾注释**：新增的 `sdkconfig.defaults.radar_bridge_edv11p` 里所有注释都独占一行，文件头写明了这条限制。

### 5.4 已用 kconfgen 实测校验（不依赖完整构建）

```powershell
$py -m kconfgen --list-separator=semicolon --kconfig <临时Kconfig: source main/Kconfig.projbuild> `
    --config $env:TEMP\kcfg\sdkconfig `
    --defaults <demo选择文件> --defaults sdkconfig.defaults.radar_bridge_edv11p `
    --output config $env:TEMP\kcfg\sdkconfig.out --output header $env:TEMP\kcfg\sdkconfig.h
```

结果（exit 0，无 error）：`CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P=y`、`CONFIG_LOG_DEFAULT_LEVEL_NONE=y`、
`# CONFIG_APP_EDK60_STARTUP_BANNER is not set`、`# CONFIG_APP_EDK60_PRINT_PINMAP is not set`、
`CONFIG_APP_EDK60_EDV11P_BAUD=921600`、`..._SILENT_LOGS=y`、`..._UART0_DEBUG is not set`、
`..._STATS_SEC=10`、`..._UART_RX_BUF=16384`、`..._CHUNK=1024`。
（`LOG_DEFAULT_LEVEL_NONE` 在这次最小 Kconfig 里被报为 unknown symbol，是因为临时 harness 没有 include IDF 自身的 Kconfig；真实构建里它是已知符号。）

---

## 六、构建结果

### 6.1 单 demo 构建（由项目经理在提权环境执行）

| 项 | 结果 |
|---|---|
| 命令 | `idf.py -B %TEMP%\edk60-b11p -D IDF_TARGET=esp32c6 -D SDKCONFIG=%TEMP%\sdkconfig.b11p -D SDKCONFIG_DEFAULTS="<repo>\sdkconfig.defaults;<repo>\sdkconfig.defaults.esp32c6;<repo>\sdkconfig.defaults.radar_bridge_edv11p" build` |
| 结果 | **BUILD_EXIT=0** |
| `edk60_demos.bin` | **0x2B740 = 177 984 B ≈ 174 KB** |
| 分区余量 | 单应用分区 1 MB，剩余 **83%** |
| 备注 | 该次构建对应**本文件 P1 修改前的源码**；补了 3 个计数器与 60 ms 预算后需再构建一次（见 6.4） |

### 6.2 合并固件（merge-bin）

| 项 | 结果 |
|---|---|
| 命令 | `idf.py -B build-radar_bridge_edv11p merge-bin` |
| 产物 | `build-radar_bridge_edv11p\merged-binary.bin` |
| 体积 | **待补**（由项目经理执行后回填） |
| SHA256 | **待补**（`Get-FileHash -Algorithm SHA256`） |
| 烧录偏移 | **0x0**（合并固件自带 bootloader + 分区表 + app，`esptool.py write_flash 0x0 merged-binary.bin`） |

### 6.3 全量回归（17/17）

| 项 | 结果 |
|---|---|
| 命令 | `.\build_all.ps1 -Size` |
| 期望 | 17 个 demo 全部 `OK`，0 error |
| 实测 | **待补**（本会话受 DSH 沙箱限制无法执行，见 6.5） |

### 6.4 需要项目经理执行的命令（按顺序）

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos

# 1) 单 demo（最终版源码，含 P1 计数器）
.\build_all.ps1 -Demo radar_bridge_edv11p

# 2) 全量回归（17/17）
.\build_all.ps1 -Size

# 3) 合并固件 + 体积 + SHA256
idf.py -B build-radar_bridge_edv11p merge-bin
Get-Item build-radar_bridge_edv11p\merged-binary.bin | Select-Object Length
Get-FileHash build-radar_bridge_edv11p\merged-binary.bin -Algorithm SHA256
```

> 请把 2)、3) 的结果回填到 6.2 / 6.3 两张表 —— 那是 Gate-2 的硬证据。

### 6.5 为什么本会话跑不了构建（环境事实，非代码问题）

| 尝试 | 现象 | 结论 |
|---|---|---|
| `.\build_all.ps1 -Demo ...` | `无法加载文件 build_all.ps1，因为在此系统上禁止运行脚本` | 执行策略；用 `Set-ExecutionPolicy -Scope Process Bypass` 绕过（脚本逻辑不改） |
| 同上（已绕过） | `Your environment is not configured to handle Unicode characters.` / Python traceback | 需 `PYTHONIOENCODING=utf-8`、`PYTHONUTF8=1` |
| `idf.py build`（已设 UTF-8） | `PermissionError: [WinError 5] 拒绝访问` at `asyncio → windows_utils.pipe() → _winapi.CreateFile` | DSH 沙箱禁止创建**命名管道**；`idf.py` 用 asyncio 管道捕获 cmake/ninja 输出，必然失败 |
| `cmake` + `ninja` 直接调用 | cmake 配置阶段停住（进程无进展） | 同一沙箱限制的间接表现；已终止这些子进程 |

**本会话替代验证手段**（都已跑过，结论见下）：

| 验证 | 手段 | 结果 |
|---|---|---|
| C 源码语法/警告 | `riscv32-esp-elf-gcc ... -fsyntax-only`（复用 `build-hello_board/compile_commands.json` 的真实参数：`-Wall -Wextra -Werror -std=gnu23`） | **5 种配置全部 exit 0、输出 0 字节（0 error / 0 warning）**：<br>①`radar_bridge_edv11p.c` 静默 + 无诊断（量产配置）<br>②同上 + `UART0_DEBUG=y`（诊断分支参与编译）<br>③同上但**未选中本 demo**（等价于其它 16 个 demo 的编译条件 ⇒ 证明没有 `#error` 泄漏）<br>④`demo_dispatch.c` 选中 `EDV11P`（走新分支 + 横幅门禁）<br>⑤`demo_dispatch.c` 选中 `hello_board`（其它 demo 路径） |
| Kconfig 语法与 defaults 叠加 | `python -m kconfgen`（纯 Python，无子进程管道） | **exit 0**，第五节 5.4 列出的 10 项全部按预期生效 |

---

## 七、烧录与联调步骤

### 7.1 接线

| 端 | 接法 |
|---|---|
| 雷达模组 | 插 **H3**（`RES A R T G V P`）：模组 TX → H3 的 **T**（IO4），模组 RX ← H3 的 **A**（IO5），电源取 H3 的 P/V。**不要插 H4**（H4 的 T 与 EN 同网络，电源与数据互相干扰） |
| 电源使能 | IO14 由固件拉高（丝印 **RADAR 的红灯亮** = 3.3V_RADAR 已上电） |
| PC | USB-C 直连电脑；出现「USB 串行设备 (COMx)」（Linux `/dev/ttyACM0`）。板上**没有** CH340/CP2102 |
| 可选诊断 | USB-TTL 接 H2 的 D0(IO16)/D1(IO17)，115200，8N1（仅在 `UART0_DEBUG=y` 时有输出） |

### 7.2 编译 / 烧录

```powershell
.\build_all.ps1 -Demo radar_bridge_edv11p          # 或 idf.py -B build-radar_bridge_edv11p build
idf.py -B build-radar_bridge_edv11p -p COM6 flash  # 开发阶段：直接烧
# 发布用合并固件：
idf.py -B build-radar_bridge_edv11p merge-bin      # → merged-binary.bin，烧 0x0
python -m esptool --chip esp32c6 -p COM6 -b 460800 write_flash 0x0 merged-binary.bin
```

### 7.3 上位机联调

1. 打开 COM 口（波特率 921600，8N1，无流控；USB CDC 实际不受波特率限制）。
2. **注意**：打开端口时若上位机拉 DTR/RTS，板子会复位一次（pitfalls D3），1~2 秒后恢复；
   上位机应在拿到第一帧前保持重试（`GET 0x02` 可重复发）。
3. 先回读版本：`GET 0x02` → 帧 `45 44 00 0B 00 01 A0 02 00 00 37`（示例），**保存原始字节**留档。
4. 下发工作模式：`type 0x05` 设为 `1`（跟踪/人数统计）。
5. 下发上报周期：`type 0x10`，**单位 50 ms**，演示建议 `2`（= 100 ms）。
6. 观察轨迹与人数。

---

## 八、已知限制

| # | 限制 | 影响 | 处置 |
|---|---|---|---|
| 1 | 复位瞬间可能落几行 IDF 启动日志到 USB | 上位机看到一次「尾巴」 | 不含 `0x45 0x44` 相邻（横幅已关）；上位机的帧同步会跳过 |
| 2 | 打开串口可能触发板子复位（DTR/RTS 硬件行为） | 刚开始的 1~2 秒无数据 | 上位机重试；不要用「一打开就必须立刻有数据」的判据 |
| 3 | 上位机若长时间（>60 ms）不读 USB，本批会丢字节**但会计数** | 32 分钟级偶发丢帧 | 上位机侧保持连续读；用 `usb_tx_dropped` 判定 |
| 4 | 雷达 EN 不做运行期控制 | 无法软件复位雷达/省电 | 有意为之：运行中误拉低 EN 会「突然没数据」（历史坑）。需要断电时手动拔插 |
| 5 | 双向同时满载时，USB→UART 方向共用 UART1 TX，延迟 ≈ 帧长/波特率 | 上位机 SET 命令的应答延迟数 ms | 可接受；不做优先级调度 |
| 6 | 不校验、不重传、不缓存断链期间的数据 | 语义就是「透明管道」 | 由上位机协议层负责（帧头+长度+校验和） |
| 7 | 未做 USB 拔出/插入检测 | 拔线后写失败会进入退避循环（计数可见） | 插回后自动恢复，无需复位 |
| 8 | 固件不实现 0x70 目标上报的解析 | 轨迹/人数在上位机侧解析 | 设计如此（纯桥接）；解析由 `human_track_v1.4_side_mount.py` 与网页版负责 |

---

## 九、上板后 60 秒的预期现象（验收口径）

| 时间 | 期望 |
|---|---|
| 0~2 s | 插 USB：设备管理器出现「USB 串行设备」；丝印 RADAR 红灯亮（= IO14 已拉高） |
| 0~10 s | 上位机打开端口（可能触发一次复位）→ 开始收字节；流里**没有中文/ASCII 日志文本** |
| 10~60 s | 帧流连续（100 ms/帧）；上位机 UI 出现「人数」与「目标轨迹」；人走动时轨迹同步移动 |
| 全程 | `uart_rx_overflows = 0`、`usb_tx_dropped = 0`（用 UART0 诊断通道确认） |

若 60 秒什么都没有，按顺序查：
① 红灯亮不亮（EN/电源）→ ② 模组是否插在 **H3**、T/A 是否接对 → ③ 端口是不是那个「USB 串行设备」→
④ 打开 `UART0_DEBUG`，用 USB-TTL 看 `radar->pc` 计数是否为 0 → ⑤ 波特率是否被改过（921600）。

---

## 十、留给下一步（不属于 T1-1~T1-3）

| # | 事项 | 责任 |
|---|---|---|
| 1 | 执行 6.4 的三条命令并回填 6.2 / 6.3 | 项目经理（提权环境） |
| 2 | `tools/edv11p_frame_check.py --raw-dump` 证明 USB 流无非协议字节 | 测试工程师 |
| 3 | 上板 30 分钟帧完整率 ≥99.9% + `uart_rx_overflows==0` | 测试工程师 |
| 4 | `GET 0x02` 版本回读结果归档（决定 0x70 上报是否可用） | 测试工程师 + 硬件 |
| 5 | 若要在**量产固件**里保留诊断能力，评估是否把统计改为可选 GPIO 指示（LED 闪码） | 嵌入式（下一轮） |
