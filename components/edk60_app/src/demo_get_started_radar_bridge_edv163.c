/*
 * board K60 Demo 5.3 —— radar_bridge_edv163（水位监测雷达 · 纯字节透传桥）
 * ===========================================================================
 * 30 秒看懂这个 demo
 * ---------------------------------------------------------------------------
 *   干什么 : 把插在 H3 排针上的 EDV163 雷达串口数据（**256000 8N1**）
 *            **原样、双向**搬到 ESP32-C6 的原生 USB Serial/JTAG（USB-C 口）。
 *            PC 端上位机（网页 Web Serial / Python）直接打开那个
 *            「USB 串行设备 (COMx)」就能自己组帧/解析/换算水位 ——
 *            上位机自己会发 GET/SET 配置帧，本固件不参与。
 *   不干什么: **不解析任何协议、不组帧、不发任何命令、不改写字节流**。
 *            不加换行、不加前缀、不做转义/反转义、不做十六进制可视化、
 *            不做私有三段式封包、**不计算水位**。
 *            （上一代 EDV151 那套开机六条握手 + 私有帧在这里**完全不适用**；
 *              EDV11P 的人数统计帧在这里同样不适用。）
 *   怎么验 : 见下面「上板验证（60 秒看什么）」。
 *
 * 数据通路
 * ---------------------------------------------------------------------------
 *     雷达模组 ──UART1──► ESP32-C6 ──原生 USB Serial/JTAG──► PC(COMx)
 *      TX(IO4)   RX(IO4)              IO12(D-)/IO13(D+)
 *      RX(IO5) ◄─TX(IO5)  ◄── 上位机发的 GET/SET 帧原样下发
 *
 *   引脚全部取自 BSP（components/edk60_board/include/edk60_board.h，含静态断言）：
 *     · 模组 TX -> MCU RX = IO4   （H3 的 T 脚；模组必须插 H3，H4 的 T 与 EN 同网）
 *     · MCU TX -> 模组 RX = IO5   （H3 的 A 脚）
 *     · 雷达电源使能 EN   = IO14  （输出高 = 3.3V_RADAR 上电；丝印 RADAR 红灯亮）
 *     · 橙色指示灯(丝印 L) = IO15 （**仅指示灯**，本 demo 完全不碰它）
 *   串口参数：**256000 / 8N1 / 无流控**（Kconfig 可改，默认即 256000）。
 *   ⚠ 与 5.2（EDV11P，921600）不同：**这里不是 921600**。波特率错一档的典型
 *     现象是「一个字节都收不到」（帧错误让驱动整体丢字节，见 pitfalls E2），
 *     而不是「收到乱码」——所以上板第一件事就是核对帧头 0x45 0x44 有没有出现。
 *
 * ===========================================================================
 * EDV163 协议速查（**仅作文档，固件一行都不实现**）
 * ===========================================================================
 *   依据：repo/开源产品线/esp_components_registry/ed_radar/docs/EDV163串口通用协议.md
 *   （V2.0 固件，默认设备 ID `EDV163_VB`，默认波特率 256000）
 *
 *   帧：0x45 0x44 | len(2,BE) | msgid(2,BE) | op(1) | type(1) | tlvlen(2,BE)
 *       | data | checksum(1) | 0x0A
 *     · len      = 整包字节数（**转义前**）= 12 + data 长度，最小 12、最大 256
 *     · checksum = 转义前从帧头到 data 末逐字节求和取低 8 位
 *     · 帧尾     = **固定 0x0A**（帧尾本身不参与转义）
 *     · 转义     = 正文里 0x0A -> 0x09 0x01、0x09 -> 0x09 0x00；接收方需还原
 *                  （**这是本项目此前所有模组都没有的一层**，最容易照抄漏掉）
 *     · op       = 0xA0 GET / 0xA1 SET / 0xA2 REPORT / 0xA3 STATUS / 0xA9 ACK
 *     · type     = 0x01 产品信息、0x02 模块信息、0x11 检测延时、
 *                  0x17 上报频率(单位 **100 ms**)、0x18 工作模式(0=关/1=正常)、
 *                  0x25 挂高(2B mm，1200~5000)、0x26 检测高度、0x27 检测范围、
 *                  0x28 自动测高、0x29 屏蔽区、0x81 上报数据
 *     · 0x81 载荷 = count(1B) + count × 8B{ channel, delay_presence,
 *                   target_state, motion_direction,
 *                   target_distance(2B BE, mm), target_angle(1B 有符号) }
 *     · 水位换算（**上位机做**）= 挂高_mm − target_distance_mm
 *
 *   ⚠ 以上内容在固件里**没有任何对应代码**，写在这里只为两件事：
 *     ① 让评审/接手的人一眼看出「桥的边界」——这些字节我们只管搬，不管懂；
 *     ② 让上板验证的人知道该在 USB 流里找什么（0x45 0x44 开头的帧）。
 *
 * ===========================================================================
 * 控制台洁净方案（本项目最高优先风险，结论 + 证据 + 怎么验证）
 * ===========================================================================
 * 问题：ESP-IDF 在 C6 上默认把控制台日志同时输出到「主控制台 UART0」和
 *       「第二输出口 USB Serial/JTAG」。而 USB 上跑的正是雷达二进制数据流，
 *       两者写进同一个 USB FIFO ⇒ 上位机收到「数据夹日志」的混合流。
 *       更糟的是日志里的板名以两个 ASCII 字节 "E""D" 开头（0x45 0x44），
 *       而 EDV163 的帧头**正好就是 0x45 0x44** ⇒ 上位机会把日志当成一帧的
 *       开头（假同步），紧跟着按 len 字段跳字节，整段解析全错位。
 *       （EDV11P 的帧头同样是 0x45 0x44，所以这条风险完全继承，不能因为
 *         「换了模组」就放松。）
 *
 * 方案取舍（三个候选，逐个给出结论，依据是本仓库的上板实测记录）：
 *
 *  ① 把主控制台从 USB 挪走 → **不需要做**：本工程主控制台**本来就是 UART0**
 *       （H2 的 D0=IO16 / D1=IO17，115200），USB 只是「第二输出口」。
 *       证据：components/edk60_app/src/demo_get_started_usb_serial_bridge.c
 *       头部注释「控制台（UART0，115200-8-N-1，**不受本 demo 影响**）」。
 *       并且 docs/EDK60-pinmap.md 第五节：UART0 已引到 H2 的 D0/D1，
 *       与雷达占用的 IO4/IO5、EN=IO14 完全不冲突 —— 但既然现状已经是 UART0，
 *       再改一次只会增加风险，不做。
 *
 *  ② 关掉「USB 第二输出口」（CONFIG_ESP_CONSOLE_SECONDARY_NONE=y）→ **禁止**。
 *       实测（2026-10-15）：固件烧入后系统启动**直接卡死**，日志停在
 *       `sleep_gpio: Enable automatic switching of GPIO sleep configuration`，
 *       连 main_task 都不启动，USB 口不再枚举，**必须物理断电重插**。
 *       根因：该选项与 USJ_ENABLE_USB_SERIAL_JTAG 联动，两个控制台都从 USB
 *       撤掉后 USB 外设时钟在启动阶段被关死。
 *       记录位置：sdkconfig.defaults.radar_bridge 文件头、docs/EDK60-pitfalls.md D1。
 *       本文件末尾对此有**编译期断言**（见「编译期保护」一节）。
 *
 *  ③ 【本 demo 采用】保持 console 配置**完全不动**，用「不产生字符」实现洁净：
 *       a) sdkconfig.defaults.radar_bridge_edv163 里设 CONFIG_LOG_DEFAULT_LEVEL_NONE=y
 *          ⇒ ESP_LOG* 在**编译期**就被消掉，一个字符都不会写出去（CONFIG 级保险）；
 *       b) 关闭启动横幅与引脚表（CONFIG_APP_EDK60_STARTUP_BANNER /
 *          CONFIG_APP_EDK60_PRINT_PINMAP），它们走 printf —— 实测过「只关日志级别」
 *          挡不住 printf：radar_bridge 的实测记录里抓到 1142 字节控制台文本；
 *          而且横幅里的板名正好以 0x45 0x44 开头，会制造假同步；
 *       c) 本文件**通篇没有任何 printf / ESP_LOGx**（唯一的诊断输出走 d) 之外的
 *          第四条通道，见 d）；
 *       d) 需要诊断时用 APP_EDK60_EDV163_UART0_DEBUG=y：**直写 UART0**（ROM 单字节
 *          输出，绕过 esp_log / stdio，因此**不会**被复制到 USB），输出文案里也
 *          刻意不出现以 "E""D" 开头的词（用 "k60" / "radar" 代替），双重保险；
 *       e) 初始化第一步就调用 esp_log_level_set("*", ESP_LOG_NONE)：
 *          给「用户用 menuconfig 自己编、没走 demo 专用 defaults」的情况兜底，
 *          顺手压住驱动层的运行期日志（CONFIG 级保险失效时的第二道）。
 *
 * 为什么 d) 不用 uart_write_bytes(UART_NUM_0, ...)：该 API 要求 UART0 驱动已安装，
 * 而 console 并不安装 UART0 驱动（它走 ROM 直写）。为了打印诊断而在 IO16/IO17 上
 * 装一个 driver，等于把「控制台 + 驱动」两套写者放在同一根 TX 上，收益极低、
 * 风险不小。esp_rom_output_tx_one_char() 只写**主控制台**（UART0），
 * 不受 second console 复制逻辑影响 —— 依据是 IDF 源码
 * components/esp_stdio/stdio_simple.c：
 *      esp_system_console_put_char(c) = esp_rom_output_tx_one_char(c)
 *                                     [+ esp_rom_usb_serial_putc(c) 仅当
 *                                        CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG]
 * 也就是说「复制到 USB」发生在 stdio 层，ROM 单字节输出这一层没有复制。
 *
 * 验证方法（怎么证明 USB 上真的干净）：
 *   · 静态：PowerShell 里 `Select-String -Path sdkconfig.radar_bridge_edv163
 *     -Pattern "LOG_DEFAULT_LEVEL_NONE|STARTUP_BANNER|PRINT_PINMAP"` 三个都要命中；
 *   · 动态：PC 上用 tools/edv163_frame_check.py（测试工程师提供）抓 30 秒，
 *     要求「无法解析的字节数 = 0 / 未归属字节率 = 0」；
 *   · 已知残留：**上电/复位瞬间**（本 demo 第一行代码之前的 IDF 启动日志）仍可能
 *     有几行落到 USB。端口打开时 DTR/RTS 常常触发一次复位，所以上位机可能看到
 *     一次启动尾巴。这些字节里没有 0x45 0x44 相邻（横幅已关），上位机的
 *     帧同步（帧头 + 长度 + 校验和 + 帧尾 0x0A）会直接跳过它们。
 *
 * ===========================================================================
 * 缓冲区 / 超时 / 丢包：算清楚，且**任何丢字节都必须有计数**
 * ===========================================================================
 *   线速换算：256000 8N1 = 10 bit/byte ⇒ **25600 B/s**（≈25 KB/s），
 *             1024 B 的线上时间 ≈ **40.0 ms**，单帧最大 256 B ≈ **10.0 ms**
 *             （最坏情况整帧 0x0A/0x09 全中，转义后 ≈ 512 B ≈ 20 ms）。
 *             —— 注意这是 EDV11P（92160 B/s）的 1/3.6，所以本 demo 的缓冲
 *                余量比 5.2 更大，不需要为吞吐做任何特殊优化。
 *
 *   约束关系（关键，别写含糊的「余量很大」）：
 *     ① USB 写单批最多耗 BR163_USB_WR_BUDGET_MS = 60 ms（超预算就放弃剩余字节并计数）；
 *     ② UART1 RX 环形缓冲默认 16384 B ÷ 25600 B/s ≈ **640.0 ms** 的流量；
 *     ③ 于是「任务被 USB 写拖住」的最长时间 60 ms ≪ 「环形缓冲撑住」的 640 ms，
 *        比值 ≈ **10.67×**（EDV11P 是 2.96×）。这是**唯一的定量保证**：只要上位机
 *        在 60 ms 内恢复读取，一个字节都不会丢；超过这个界限一定会有丢字节，
 *        但**不会静默**——USB 侧记 usb_tx_dropped / usb_tx_short，
 *        UART 侧记 uart_rx_overflows。
 *     ④ 文件末尾有一条**编译期断言**把这个不等式钉死：任何人把波特率调高、
 *        或把环形缓冲调小到破坏 2× 关系时，构建直接失败并给出算式
 *        （断言只在选中本 demo 时生效，见「编译期保护」）。
 *
 *   各缓冲区取值与理由：
 *   · UART1 RX 环形缓冲 16384 B（Kconfig 可调 1~32 KB）：见上面 ②。
 *     为什么与 5.2 保持同样的 16384 而不再缩小：本板 SRAM 512 KB，多 8 KB
 *     换「上位机线程被调度走 640 ms 也不丢字节」的余量，非常划算；
 *     缩小到 1024 B 时只有 40 ms 深度，就只剩 0.67×，会被上面的断言拦下。
 *   · USB TX 环形缓冲 16384 B：≈ 640 ms 的突发量（同样按 25600 B/s 算），
 *     上位机（Python / 浏览器 Web Serial）偶发被调度走 100 ms 很常见，
 *     这个池子让「短停顿」直接吸收掉，不会走到丢字节那一步。
 *   · USB RX 环形缓冲 4096 B：上位机的配置帧很小（单帧 ≤ 256 B，转义后
 *     ≤ 512 B；开机批量 GET/SET 十几帧约 2~4 KB）且在 100 ms 内突发下发；
 *     USB→UART 任务以 10 ms 级轮转，4096 B 足以承接这一波。（USB 是主机驱动的
 *     事务传输，主机侧不会因为设备缓冲满而丢字节，最坏是本事务 NAK 后重发。）
 *   · UART1 TX 缓冲 4096 B：uart_write_bytes 会阻塞到全部入队，天然形成反压。
 *
 *   可观测计数器（全部通过 UART0 诊断通道输出，默认关闭；**诊断开启时也不写 USB**）：
 *     uart_rx_overflows  UART 环形缓冲满事件次数（= 有字节被驱动丢弃）
 *     uart_fifo_ovf      硬件 FIFO 溢出次数（ISR 来不及搬）
 *     uart_frame_errs    帧错误次数（**波特率不对的典型症状**，本 demo 特别注意
 *                        256000 是否被改错）
 *     usb_tx_dropped     因 USB 写超预算而放弃的字节数
 *     usb_tx_short       发生「短写」的批次数
 *     usb_wr_stalls      USB 写受阻（环形缓冲满/驱动异常）的次数
 *   ⇒ 判据：uart_rx_overflows == 0 且 usb_tx_dropped == 0 且 uart_frame_errs == 0
 *     才叫「零丢字节 + 波特率正确」。
 *   · 单次搬运块 1024 B（Kconfig 可调）：≈40 ms 线时，是最大帧（转义后 ≤512 B）
 *     的 2 倍，既减少循环次数也不会让单批过大挤压 USB 缓冲。
 *   · UART 驱动事件队列 32 项：数据通路本身走环形缓冲，事件队列只是
 *     给驱动留的余量（本 demo 消费事件只为统计，满不影响数据落进环形缓冲）。
 *
 *   任务：
 *     r2u（雷达→USB）优先级 6：唯一写 USB 的任务，因此 USB 侧**不需要加锁、也不会撕裂**。
 *     u2r（USB→UART）优先级 6：把上位机的帧原样写进 UART1。
 *     brstat（仅 UART0_DEBUG=y 时创建）优先级 3：每 N 秒往 UART0 打一行统计。
 *   两个搬运任务的栈 3072 B，收发缓冲都放在静态区（不进栈）。
 *
 * 背压与丢包策略（与上面的定量关系一一对应，不许含糊）
 * ---------------------------------------------------------------------------
 *   · USB 写：单批最多耗 60 ms（多次重试共享这一个预算）。超预算 ⇒ 放弃该批
 *     剩余字节，并分别计入 usb_tx_dropped（字节）和 usb_tx_short（批次）。
 *     为什么不「无限重试」：无限重试会让搬运任务停摆任意长时间，而 UART 环形
 *     缓冲只有 640 ms 的深度 —— 那才是真正的静默丢字节。
 *   · UART 写：驱动带 4096 B TX 环形缓冲，uart_write_bytes 会阻塞到全部入队，
 *     天然反压上位机，不丢字节。
 *   · UART 收：环形缓冲溢出与硬件 FIFO 溢出都通过 UART 事件队列统计（见上）。
 *   · 电源使能脚 IO14：拉高后**全程不再动**（历史坑：运行中误拉低会让雷达掉电，
 *     现象是「突然没数据」，看起来像通信问题，实际是自己把电断了）。
 *
 * ===========================================================================
 * 上板验证（60 秒看什么）
 * ===========================================================================
 *   1) 硬件：模组插 **H3**（不是 H4）；USB-C 接电脑；上电后丝印 RADAR 的红灯亮
 *      （= 3.3V_RADAR 已使能，由 IO14 控制）。
 *   2) PC 上出现「USB 串行设备 (COMx)」。波特率填 256000（USB CDC 其实不依赖
 *      波特率，填一致只为与模组侧对齐、便于对数）。
 *   3) 打开端口后若上位机拉 DTR/RTS，板子会复位重启一次（工程已知现象，见
 *      docs/EDK60-pitfalls.md D3）—— 重启后 1~2 秒内恢复。
 *   4) 60 秒内应看到：
 *      · 一个字节级的连续流，帧以 **0x45 0x44** 开头、以 **0x0A** 结尾；
 *      · 上位机自己下发 0x18（工作模式=1）、0x17（上报频率，单位 100 ms）、
 *        **0x25（挂高，如 2000 mm）** 之后，应周期收到 type=0x81 的上报帧；
 *      · 上位机 UI 显示「到水面距离」与「水位 = 挂高 − 距离」；
 *      · 流里没有任何中文/ASCII 日志文本（除了极少量复位尾巴）。
 *   5) 若 60 秒什么都没看到，按顺序查：红灯亮不亮 → 模组是否插在 H3 →
 *      端口是不是那个 USB 串行设备 → **波特率是不是 256000**（不是 921600）→
 *      用 UART0_DEBUG 打开统计看 uart_rx 是否为 0、uart_frame_errs 是否在涨。
 *
 * ===========================================================================
 * 编译期保护（本工程 18 个 demo 一起编译，断言必须绑定本 demo）
 * ===========================================================================
 *   见文件末尾：任何 #error 都写在
 *     #if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163) && ...
 *   里面。若漏掉前半段，会让其余 17 个 demo 全部构建失败（历史坑 B1）。
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_serial_output.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* 日志级别被编译成 NONE 时，TAG 会「看起来没被用到」；显式标 unused 避免告警。
 * 注意 TAG 不以 "E""D" 开头（0x45 0x44 = EDV163 帧头），
 * 且本文件在静默模式下不输出任何字符。 */
static const char *TAG __attribute__((unused)) = "bridge163";

/* ===========================================================================
 * 配置映射（Kconfig → 本文件的编译期常量；引脚一律取自 BSP，禁止硬编码）
 * =========================================================================== */

#define BR163_UART_PORT     UART_NUM_1
#define BR163_RX_PIN        EDK60_PIN_RADAR_UART_RX   /* IO4：模组 TX -> MCU */
#define BR163_TX_PIN        EDK60_PIN_RADAR_UART_TX   /* IO5：MCU -> 模组 RX */
#define BR163_PWR_EN_PIN    EDK60_PIN_RADAR_PWR_EN    /* IO14：输出高 = 雷达上电 */

#define BR163_BAUD          ((uint32_t)CONFIG_APP_EDK60_EDV163_BAUD)
#define BR163_CHUNK         (CONFIG_APP_EDK60_EDV163_CHUNK)
#define BR163_UART_RX_BUF   (CONFIG_APP_EDK60_EDV163_UART_RX_BUF)
#define BR163_STATS_SEC     (CONFIG_APP_EDK60_EDV163_STATS_SEC)

/* UART1 发送缓冲：上位机的 GET/SET 帧很小，4 KB 足够；驱动会阻塞到全部入队 */
#define BR163_UART_TX_BUF       4096
/* UART 事件队列项数（本 demo 只消费事件做统计，给驱动留余量即可） */
#define BR163_UART_EVT_QUEUE    32

/* USB 缓冲：见文件头「缓冲区/任务结构」的定量说明 */
#define BR163_USB_TX_BUF        16384
#define BR163_USB_RX_BUF        4096

/* 超时与退避（与文件头「缓冲区/超时/丢包」的定量关系绑定，改这里必须同步改注释） */
#define BR163_UART_RD_TIMEOUT_MS    10   /* 无数据时每 10 ms 轮一次，25 KB/s 下攒 ~256 B */
#define BR163_USB_RD_TIMEOUT_MS     10
#define BR163_USB_WR_ATTEMPT_MS     10   /* 单次 USB 写最多等多久 */
#define BR163_USB_WR_BUDGET_MS      60   /* 单批数据在 USB 写里的总预算（必须 < UART RX 缓冲深度） */
#define BR163_USB_WR_BACKOFF_MS     2

#define BR163_TASK_STACK        3072
#define BR163_TASK_PRIO         6
#define BR163_STAT_TASK_PRIO    3
#define BR163_STAT_TASK_STACK   3072

/* Kconfig 的 bool 在「未选中」时**根本不生成宏**（本项目踩过 3 次），
 * 所以凡「用户可以关掉」的 bool 一律用 #ifdef 兜底。 */
#ifdef CONFIG_APP_EDK60_EDV163_SILENT_LOGS
#define BR163_SILENT            1
#else
#define BR163_SILENT            0
#endif

#ifdef CONFIG_APP_EDK60_EDV163_UART0_DEBUG
#define BR163_UART0_DBG         1
#else
#define BR163_UART0_DBG         0
#endif

/* 验收取证专用（沿用 5.2 的 Gate-2 P1-03 做法）：把「诊断/统计」改从 **USB** 输出，
 * 用于在没有 UART0 串口板的情况下读取 uart_rx_overflows / usb_tx_dropped /
 * uart_frame_errs 这几个背压与波特率计数器。会让数据流混入文本，**仅供一次性取证**，
 * 正式固件必须保持关闭（默认 n）。 */
#ifdef CONFIG_APP_EDK60_EDV163_STATS_VIA_USB
#define BR163_STATS_VIA_USB     1
#else
#define BR163_STATS_VIA_USB     0
#endif

#if (BR163_UART0_DBG || BR163_STATS_VIA_USB)
#define BR163_DBG_ENABLED       1
#else
#define BR163_DBG_ENABLED       0
#endif

/* ===========================================================================
 * 统计（两个搬运任务都会写，用临界区保护；只用于诊断通道输出）
 * =========================================================================== */

typedef struct {
    uint32_t uart_rx_bytes;     /* 雷达 → 板 的字节数（已进 UART 环形缓冲） */
    uint32_t uart_rx_chunks;    /* 上述方向的搬运次数 */
    uint32_t uart_rx_overflows; /* UART 环形缓冲满事件数 ⇒ 有字节被驱动丢弃（关键判据） */
    uint32_t uart_fifo_ovf;     /* 硬件 FIFO 溢出事件数 */
    uint32_t uart_frame_errs;   /* 帧错误事件数（波特率不对/干扰的典型症状，关键判据） */
    uint32_t usb_tx_bytes;      /* 板 → PC 的字节数（真正写进 USB TX 环形缓冲） */
    uint32_t usb_tx_dropped;    /* 因 USB 写超预算而放弃的字节数（关键判据） */
    uint32_t usb_tx_short;      /* 发生「短写」的批次数 */
    uint32_t usb_wr_stalls;     /* USB 写受阻（缓冲满/驱动异常）的次数 */
    uint32_t usb_rx_bytes;      /* PC → 板 的字节数 */
    uint32_t usb_rx_chunks;
    uint32_t uart_tx_bytes;     /* 板 → 雷达 的字节数 */
} br163_stats_t;

static br163_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

/* UART 事件队列：本 demo 不靠事件搬数据（数据走环形缓冲），但**必须**消费事件，
 * 否则没法统计「环形缓冲满 / FIFO 溢出 / 帧错误」—— 那正是「有没有静默丢字节」
 * 与「波特率设对没有」的唯一可验证证据。 */
static QueueHandle_t s_uart_evt_q;

/* 收发缓冲放静态区：任务栈只放局部变量，省 RAM 也更抗栈溢出 */
static uint8_t s_buf_r2u[BR163_CHUNK];  /* 雷达 → USB */
static uint8_t s_buf_u2r[BR163_CHUNK];  /* USB → 雷达 */

static inline void stats_add(uint32_t *field, uint32_t n)
{
    if (n == 0) {
        return;
    }
    portENTER_CRITICAL(&s_stats_lock);
    *field += n;
    portEXIT_CRITICAL(&s_stats_lock);
}

/* ===========================================================================
 * 诊断输出通道（默认关闭）：直写 UART0，不经过 esp_log / stdio，因此不会被
 * 复制到 USB 第二输出口。文案刻意避开以 "E""D" 开头的词（见文件头 ③d）。
 * =========================================================================== */

#if BR163_DBG_ENABLED
static void dbg_raw(const char *s)
{
#if BR163_STATS_VIA_USB
    /* 取证模式：直写 USB CDC（与雷达数据共用端点，故意制造可测量的污染） */
    size_t len = strlen(s);
    if (len != 0) {
        (void)usb_serial_jtag_write_bytes((const uint8_t *)s, len, pdMS_TO_TICKS(50));
    }
#else
    while (*s != '\0') {
        (void)esp_rom_output_tx_one_char((uint8_t)(unsigned char)*s);
        s++;
    }
#endif
}

static void dbg_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void dbg_line(const char *fmt, ...)
{
    char line[192];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }
    size_t len = (n < (int)sizeof(line)) ? (size_t)n : (sizeof(line) - 1);
#if BR163_STATS_VIA_USB
    /* 取证模式：整行都走 USB（5.2 曾只把行尾 \r\n 走了 USB，正文仍进 UART0，
     * 结果取回的文件里只剩一串空行 —— 2026-10-22 实测踩到，这里不再重复） */
    (void)usb_serial_jtag_write_bytes((const uint8_t *)line, len, pdMS_TO_TICKS(50));
#else
    for (size_t i = 0; i < len; i++) {
        (void)esp_rom_output_tx_one_char((uint8_t)(unsigned char)line[i]);
    }
#endif
    dbg_raw("\r\n");
}
#endif /* BR163_DBG_ENABLED */

/* ===========================================================================
 * 字节搬运助手（纯字节搬运：不改写、不加前缀、不做转义、不做可视化）
 * =========================================================================== */

/**
 * 把一批字节推给 USB 主机。
 * 返回真正写进 USB TX 环形缓冲的字节数；< len 表示本批在 BR163_USB_WR_BUDGET_MS
 * 预算内没能写完（调用方负责把差额计入 usb_tx_dropped / usb_tx_short）。
 *
 * 为什么是「总预算」而不是「重试次数」：重试次数无法约束最坏停摆时间，
 * 而停摆时间一旦超过 UART 环形缓冲的深度（≈640 ms @256000），丢的就是驱动里
 * 看不见的字节。60 ms 的硬预算保证「只要主机 60 ms 内恢复读取，一个字节不丢」，
 * 超出的部分明确计数，不做静默丢弃。
 */
static size_t usb_write_all(const uint8_t *data, size_t len)
{
    size_t off = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)BR163_USB_WR_BUDGET_MS * 1000;

    while (off < len) {
        int w = usb_serial_jtag_write_bytes(data + off, len - off,
                                            pdMS_TO_TICKS(BR163_USB_WR_ATTEMPT_MS));
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        /* w == 0：TX 环形缓冲满且本次等待内没腾出空间（上位机没在读）
         * w <  0：驱动异常（例如驱动没装成功）
         * 两种情况都在预算内退避重试，预算用尽就交给调用方计数。 */
        stats_add(&s_stats.usb_wr_stalls, 1);
        if (esp_timer_get_time() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BR163_USB_WR_BACKOFF_MS));
    }
    return off;
}

/**
 * 消费 UART 事件队列，把「丢字节 / 帧错」类事件计入统计。
 * 事件类型与实际语义（driver/uart.h，uart_event_type_t）：
 *   UART_BUFFER_FULL : 环形缓冲满 ⇒ 有字节被驱动丢弃
 *   UART_FIFO_OVF    : 硬件 FIFO 溢出 ⇒ 有字节在 ISR 之前就丢了
 *   UART_FRAME_ERR   : 帧错误（停止位/波特率不符、线路干扰）
 *   UART_PARITY_ERR  : 校验错（本 demo 无校验，理论不会出现）
 *   UART_DATA 等     : 与丢包无关，直接忽略（数据本体从环形缓冲读）
 */
static void uart_drain_events(void)
{
    if (s_uart_evt_q == NULL) {
        return;
    }

    uart_event_t evt;
    while (xQueueReceive(s_uart_evt_q, &evt, 0) == pdTRUE) {
        switch (evt.type) {
        case UART_BUFFER_FULL:
            stats_add(&s_stats.uart_rx_overflows, 1);
            break;
        case UART_FIFO_OVF:
            stats_add(&s_stats.uart_fifo_ovf, 1);
            break;
        case UART_FRAME_ERR:
            stats_add(&s_stats.uart_frame_errs, 1);
            break;
        default:
            break;   /* UART_DATA / UART_PARITY_ERR / UART_BREAK 等：与丢包统计无关 */
        }
    }
}

/**
 * 把一批字节全部推给雷达模组（UART1 TX）。
 * UART 驱动带 4 KB TX 环形缓冲，uart_write_bytes 会阻塞到全部入队，
 * 因此正常情况下一次调用即完成，形成对 USB 侧的反压而不是丢字节。
 */
static size_t uart_write_all(const uint8_t *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        int w = uart_write_bytes(BR163_UART_PORT, data + off, len - off);
        if (w <= 0) {
            break;   /* 驱动异常（-1）或队列满到无法入队（0） */
        }
        off += (size_t)w;
    }
    return off;
}

/* ===========================================================================
 * 方向 1：雷达（UART1） → USB（PC）
 * 本任务是**唯一**写 USB 的任务 ⇒ 不需要互斥锁，字节流不会交叉撕裂。
 * 上位机的帧同步（帧头 0x45 0x44 + len + checksum + 帧尾 0x0A）依赖
 * 「一个字节不多、一个字节不少」，所以这里**绝不**做任何过滤或改写。
 * =========================================================================== */

static void task_radar_to_usb(void *arg)
{
    (void)arg;

    while (1) {
        int n = uart_read_bytes(BR163_UART_PORT, s_buf_r2u, BR163_CHUNK,
                                pdMS_TO_TICKS(BR163_UART_RD_TIMEOUT_MS));
        /* 先把驱动报的异常事件记账（空转也做，代价是一次非阻塞队列读取） */
        uart_drain_events();

        if (n <= 0) {
            /* 0 = 这段时间没数据；<0 = 本次没拿到 rx 锁（读超时）。都不是错误。 */
            continue;
        }

        stats_add(&s_stats.uart_rx_bytes, (uint32_t)n);
        stats_add(&s_stats.uart_rx_chunks, 1);

        size_t w = usb_write_all(s_buf_r2u, (size_t)n);
        stats_add(&s_stats.usb_tx_bytes, (uint32_t)w);
        if (w < (size_t)n) {
            /* 短写：预算内没写完 ⇒ 明确计数，不静默丢 */
            stats_add(&s_stats.usb_tx_short, 1);
            stats_add(&s_stats.usb_tx_dropped, (uint32_t)((size_t)n - w));
        }
    }
}

/* ===========================================================================
 * 方向 2：USB（PC） → 雷达（UART1）
 * 上位机自己会发 EDV163 的 GET/SET 配置帧（0x18 工作模式 / 0x17 上报频率 /
 * 0x25 挂高 …），本固件只做搬运，**不生成、不缓存、不重发、不改写**其中任何一个。
 * =========================================================================== */

static void task_usb_to_uart(void *arg)
{
    (void)arg;

    while (1) {
        int n = usb_serial_jtag_read_bytes(s_buf_u2r, BR163_CHUNK,
                                          pdMS_TO_TICKS(BR163_USB_RD_TIMEOUT_MS));
        if (n <= 0) {
            continue;
        }

        stats_add(&s_stats.usb_rx_bytes, (uint32_t)n);
        stats_add(&s_stats.usb_rx_chunks, 1);

        size_t w = uart_write_all(s_buf_u2r, (size_t)n);
        stats_add(&s_stats.uart_tx_bytes, (uint32_t)w);
    }
}

/* ===========================================================================
 * 统计任务（只有开诊断通道时才创建；否则连任务都不占 RAM）
 * =========================================================================== */

#if BR163_DBG_ENABLED
#if BR163_STATS_SEC > 0
static void task_stats(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)BR163_STATS_SEC * 1000u));

        br163_stats_t snap;
        portENTER_CRITICAL(&s_stats_lock);
        snap = s_stats;
        portEXIT_CRITICAL(&s_stats_lock);

        /* 判据写在行里，现场不用再算：
         *   帧错 0 + 溢出 0 + usb 丢 0 ⇒ 零丢字节且波特率正确。 */
        dbg_line("[k60] radar->pc %u B / %u 批 | pc->radar %u B / %u 批 | "
                 "丢字节: uart溢出 %u(fifo %u, 帧错 %u) / usb %u | "
                 "usb 短写 %u 次 / 受阻 %u 次 | en=%d",
                 (unsigned)snap.uart_rx_bytes, (unsigned)snap.uart_rx_chunks,
                 (unsigned)snap.usb_rx_bytes, (unsigned)snap.usb_rx_chunks,
                 (unsigned)snap.uart_rx_overflows, (unsigned)snap.uart_fifo_ovf,
                 (unsigned)snap.uart_frame_errs, (unsigned)snap.usb_tx_dropped,
                 (unsigned)snap.usb_tx_short, (unsigned)snap.usb_wr_stalls,
                 (int)edk60_radar_power_is_on());
    }
}
#endif /* BR163_STATS_SEC > 0 */
#endif /* BR163_DBG_ENABLED */

/* ===========================================================================
 * 入口
 * =========================================================================== */

void demo_radar_bridge_edv163_start(void)
{
    /* --- 0. 先静默：让后面所有初始化（含 BSP 与驱动自己的日志）一个字符都不出去。
     *        CONFIG_LOG_DEFAULT_LEVEL_NONE 已经在编译期消掉 ESP_LOG*，
     *        这里再压一次，是为了兜住「用户自己 menuconfig 编、没走 demo 专用
     *        defaults」的情况。 --- */
#if BR163_SILENT
    esp_log_level_set("*", ESP_LOG_NONE);
#endif

#if BR163_DBG_ENABLED
    dbg_raw("\r\n");   /* 不用 dbg_line("")：空格式串会触发 -Wformat-zero-length */
    dbg_line("[k60] demo=radar_bridge_edv163  uart1 rx=io4 tx=io5 en=io14 baud=%u",
             (unsigned)BR163_BAUD);
    dbg_line("[k60] 纯字节透传：不解析协议、不发命令、不做转义、不改写字节流");
    /* 参数一行打全：上板时照着它核对「代码里到底用的是哪组值」 */
    dbg_line("[k60] 参数: uart_rx_buf=%d B usb_tx_buf=%d B usb_rx_buf=%d B chunk=%d B "
             "usb写预算=%d ms 统计=%d s",
             BR163_UART_RX_BUF, BR163_USB_TX_BUF, BR163_USB_RX_BUF, BR163_CHUNK,
             BR163_USB_WR_BUDGET_MS, BR163_STATS_SEC);
    dbg_line("[k60] 本诊断通道直写 uart0（rom 单字节输出），不经过日志系统，"
             "不会被复制到 usb 数据流");
#endif

    /* --- 1. 雷达电源脚：先初始化成「关闭」（与硬件默认一致），
     *        等 UART 就绪后再拉高 —— 这样 EN 拉高之后到达的每个字节都能落进
     *        UART 环形缓冲，一个不丢。 --- */
    esp_err_t err = edk60_radar_power_init();
    if (err != ESP_OK) {
#if BR163_DBG_ENABLED
        dbg_line("[k60] 雷达电源脚 io%d 初始化失败: %s", (int)BR163_PWR_EN_PIN,
                 esp_err_to_name(err));
#endif
        return;
    }

    /* --- 2. USB Serial/JTAG 驱动：缓冲尽量大（PC 侧偶发停顿时的蓄水池） --- */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_cfg.tx_buffer_size = BR163_USB_TX_BUF;
        usb_cfg.rx_buffer_size = BR163_USB_RX_BUF;

        err = usb_serial_jtag_driver_install(&usb_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            /* ESP_ERR_INVALID_STATE = 已被别处安装，按成功复用处理 */
#if BR163_DBG_ENABLED
            dbg_line("[k60] usb 驱动安装失败: %s —— 透传不可用", esp_err_to_name(err));
#endif
            return;
        }
    }

    /* --- 3. UART1 @ 256000 8N1：RX=IO4、TX=IO5（ESP32-C6 的 UART1/2 没有 IOMUX
     *        默认脚，必须显式绑定）。
     *        ⚠ 256000 是 EDV163 的默认波特率；EDV11P 是 921600，别照抄 5.2。 --- */
    const uart_config_t uart_cfg = {
        .baud_rate = (int)BR163_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_driver_install(BR163_UART_PORT, BR163_UART_RX_BUF, BR163_UART_TX_BUF,
                              BR163_UART_EVT_QUEUE, &s_uart_evt_q, 0);
    if (err != ESP_OK) {
#if BR163_DBG_ENABLED
        dbg_line("[k60] uart1 驱动安装失败: %s", esp_err_to_name(err));
#endif
        return;
    }

    err = uart_param_config(BR163_UART_PORT, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(BR163_UART_PORT,
                           (int)BR163_TX_PIN, (int)BR163_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* 收到的空闲超时：让驱动及时把 FIFO 里的残字节搬进环形缓冲 */
        err = uart_set_rx_timeout(BR163_UART_PORT, 5);
    }
    if (err != ESP_OK) {
#if BR163_DBG_ENABLED
        dbg_line("[k60] uart1 配置失败: %s（rx=io4 tx=io5 是否被占用？）",
                 esp_err_to_name(err));
#endif
        return;
    }

    /* 清掉上电噪声（此刻雷达还没上电，缓冲里只可能是悬空脚噪声）。
     * 注意：这次 flush 发生在搬运任务启动**之前**，不会与 uart_read_bytes 争锁
     * （历史坑 E1：运行期 flush 会死锁）。 */
    uart_flush_input(BR163_UART_PORT);

    /* --- 4. 雷达上电：EN=IO14 拉高并**常驻**，之后运行期再也不动这个脚。
     *        （历史坑：运行中误拉低 = 雷达掉电，现象像「突然没数据」。） --- */
    err = edk60_radar_power_on();
    if (err != ESP_OK) {
#if BR163_DBG_ENABLED
        dbg_line("[k60] 雷达电源使能失败: %s", esp_err_to_name(err));
#endif
        return;
    }
    /* 刻意**不**在这里延时等待模组启动：模组启动期间的字节同样会落进 UART
     * 环形缓冲，延时只会白丢它们。搬运任务马上就开始。 */

    /* --- 5. 双向搬运任务（两个都建成功才算就绪） --- */
    BaseType_t ok1 = xTaskCreate(task_radar_to_usb, "r2u", BR163_TASK_STACK, NULL,
                                 BR163_TASK_PRIO, NULL);
    BaseType_t ok2 = xTaskCreate(task_usb_to_uart, "u2r", BR163_TASK_STACK, NULL,
                                 BR163_TASK_PRIO, NULL);

#if BR163_DBG_ENABLED
#if BR163_STATS_SEC > 0
    (void)xTaskCreate(task_stats, "brstat", BR163_STAT_TASK_STACK, NULL,
                      BR163_STAT_TASK_PRIO, NULL);
#endif
    dbg_line("[k60] 就绪：en=%d（1=雷达已上电，丝印 RADAR 红灯应亮）",
             (int)edk60_radar_power_is_on());
#endif

    if (ok1 != pdPASS || ok2 != pdPASS) {
#if BR163_DBG_ENABLED
        dbg_line("[k60] 任务创建失败（内存不足）");
#endif
        return;
    }
}

/* ===========================================================================
 * 编译期保护
 * ---------------------------------------------------------------------------
 * ⚠ 本工程 18 个 demo 的源文件**一起编译**，任何无条件的 #error 会让每一次
 *   构建（包括其它 17 个 demo）全部失败（历史坑 B1）。所以下面的断言
 *   全部以 `defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163)` 作为第一条件。
 * =========================================================================== */

/* ① 禁止用「关闭 USB 第二控制台」来让数据流变干净（会让 C6 启动卡死）。 */
#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163) && \
    defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE)
#error "radar_bridge_edv163 不允许用「关闭 USB 第二控制台」来让数据流变干净：实测会让 ESP32-C6 启动卡死、USB 不枚举、必须断电重插。请改回 CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y，并保持 CONFIG_APP_EDK60_EDV163_SILENT_LOGS=y（见 sdkconfig.defaults.radar_bridge 文件头与 docs/EDK60-pitfalls.md D1）。"
#endif

/* ② 把「UART RX 环形缓冲深度」与「USB 写单批预算」的不等式钉死。
 *    深度(ms) = UART_RX_BUF × 1000 / (BAUD / 10)，默认 16384×1000/25600 = 640 ms。
 *    要求 ≥ 2 × BR163_USB_WR_BUDGET_MS（=120 ms），否则「搬运任务被 USB 写拖住」
 *    的这段时间里雷达灌进来的字节会溢出环形缓冲 —— 即**静默丢字节**。
 *    为什么用宏而不是运行期检查：这是纯算术关系，配错应当在构建期就炸，
 *    不要留到上板时表现为「偶发丢帧」。
 *    注意：这里只能用 CONFIG_* 宏直接算，#if 里不允许出现 (uint32_t) 强转。 */
#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163) && \
    ((CONFIG_APP_EDK60_EDV163_UART_RX_BUF * 1000) / (CONFIG_APP_EDK60_EDV163_BAUD / 10) < \
     (2 * 60))
#error "radar_bridge_edv163 的缓冲区/波特率组合不安全：UART_RX_BUF×1000÷(BAUD/10) 必须 ≥ 120 ms（= 2×USB 写预算 60 ms）。默认 16384 B @256000 = 640 ms 合格；把 BAUD 调高或把 UART_RX_BUF 调小到破坏这个关系，就会在主机停顿超过预算时静默丢字节。请调大 APP_EDK60_EDV163_UART_RX_BUF 或调回波特率 256000。"
#endif
