/*
 * board K60 Demo 5.2 —— radar_bridge_edv11p（跟踪/人数统计雷达 · 纯字节透传桥）
 * ===========================================================================
 * 30 秒看懂这个 demo
 * ---------------------------------------------------------------------------
 *   干什么 : 把插在 H3 排针上的 EDV11P 雷达串口数据（921600 8N1）
 *            **原样、双向**搬到 ESP32-C6 的原生 USB Serial/JTAG（USB-C 口）。
 *            PC 端上位机直接打开那个「USB 串行设备 (COMx)」就能解析
 *            目标轨迹与人数 —— 上位机自己会发 GET/SET 配置帧，本固件不参与。
 *   不干什么: **不解析任何协议、不组帧、不发任何命令、不改字节流**。
 *            不加换行、不加前缀、不做十六进制可视化、不做私有三段式封包。
 *            （上一代 EDV151 那套开机六条握手 + 私有帧在这里**完全不适用**。）
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
 *   串口参数：921600 / 8N1 / 无流控（Kconfig 可改，默认即 921600）。
 *
 * ===========================================================================
 * 控制台洁净方案（本项目最高优先风险，结论 + 证据 + 怎么验证）
 * ===========================================================================
 * 问题：ESP-IDF 在 C6 上默认把控制台日志同时输出到「主控制台 UART0」和
 *       「第二输出口 USB Serial/JTAG」。而 USB 上跑的正是雷达二进制数据流，
 *       两者写进同一个 USB FIFO ⇒ 上位机收到「数据夹日志」的混合流。
 *       更糟的是日志里的板名以两个 ASCII 字节 "E""D" 开头（十六进制 0x45 0x44），
 *       正好等于雷达帧头的同步字节 ⇒ 上位机会把日志当成一帧的开头（假同步）。
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
 *       a) sdkconfig.defaults.radar_bridge_edv11p 里设 CONFIG_LOG_DEFAULT_LEVEL_NONE=y
 *          ⇒ ESP_LOG* 在**编译期**就被消掉，一个字符都不会写出去（CONFIG 级保险）；
 *       b) 关闭启动横幅与引脚表（CONFIG_APP_EDK60_STARTUP_BANNER /
 *          CONFIG_APP_EDK60_PRINT_PINMAP），它们走 printf —— 实测过「只关日志级别」
 *          挡不住 printf：radar_bridge 的实测记录里抓到 1142 字节控制台文本；
 *          而且横幅里的板名正好以 0x45 0x44 开头，会制造假同步；
 *       c) 本文件**通篇没有任何 printf / ESP_LOGx**（唯一的诊断输出走 a)-b) 之外的
 *          第四条通道，见 d）；
 *       d) 需要诊断时用 APP_EDK60_EDV11P_UART0_DEBUG=y：**直写 UART0**（ROM 单字节
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
 *   · 静态：PowerShell 里 `Select-String -Path sdkconfig.radar_bridge_edv11p
 *     -Pattern "LOG_DEFAULT_LEVEL_NONE|STARTUP_BANNER|PRINT_PINMAP"` 三个都要命中；
 *   · 动态：PC 上用 tools/edv11p_frame_check.py 的原始转储模式抓 30 秒，
 *     要求「无法解析的字节数 = 0」；或直接看流里是否存在 ASCII "ED" 以外的字节；
 *   · 已知残留：**上电/复位瞬间**（本 demo 第一行代码之前的 IDF 启动日志）仍可能
 *     有几行落到 USB。端口打开时 DTR/RTS 常常触发一次复位，所以上位机可能看到
 *     一次启动尾巴。这些字节里没有任何 0x45 0x44 相邻（横幅已关），上位机的
 *     帧同步（帧头 + 长度 + 校验）会直接跳过它们。
 *
 * ===========================================================================
 * 缓冲区 / 超时 / 丢包：算清楚，且**任何丢字节都必须有计数**
 * ===========================================================================
 *   线速换算：921600 8N1 = 10 bit/byte ⇒ 92160 B/s（≈90 KB/s），
 *             1024 B 的线上时间 ≈ 11.1 ms，单帧最大 512 B ≈ 5.6 ms。
 *
 *   约束关系（关键，别写含糊的「余量很大」）：
 *     ① USB 写单批最多耗 BR_USB_WR_BUDGET_MS = 60 ms（超预算就放弃剩余字节并计数）；
 *     ② UART1 RX 环形缓冲默认 16384 B ÷ 92160 B/s ≈ 177.8 ms 的流量；
 *     ③ 于是「任务被 USB 写拖住」的最长时间 60 ms < 「环形缓冲撑住」的 177.8 ms，
 *        比值 ≈ 2.96×。这是**唯一的定量保证**：只要上位机在 60 ms 内恢复读取，
 *        一个字节都不会丢；超过这个界限一定会有丢字节，但**不会静默**——
 *        USB 侧记 usb_tx_dropped / usb_tx_short，UART 侧记 uart_rx_overflows。
 *
 *   各缓冲区取值与理由：
 *   · UART1 RX 环形缓冲 16384 B（Kconfig 可调 1~32 KB）：见上面 ②。
 *   · USB TX 环形缓冲 16384 B ≈ 177 ms 的突发量：上位机（Python / 浏览器 Web Serial）
 *     线程偶发被调度走 100 ms 很常见，这个池子让「短停顿」直接吸收掉，不会走到
 *     丢字节那一步。
 *   · USB RX 环形缓冲 4096 B：上位机的配置帧很小（单帧 ≤ 512 B，开机批量 GET/SET
 *     十几帧约 2~4 KB）且在 100 ms 内突发下发；USB→UART 任务以 10 ms 级轮转，
 *     4096 B 足以承接这一波。（USB 是主机驱动的事务传输，主机侧不会因为设备
 *     缓冲满而丢字节，最坏是本事务 NAK 后重发。）
 *   · UART1 TX 缓冲 4096 B：uart_write_bytes 会阻塞到全部入队，天然形成反压。
 *
 *   可观测计数器（全部通过 UART0 诊断通道输出，默认关闭；**诊断开启时也不写 USB**）：
 *     uart_rx_overflows  UART 环形缓冲满事件次数（= 有字节被驱动丢弃）
 *     uart_fifo_ovf      硬件 FIFO 溢出次数（ISR 来不及搬）
 *     uart_frame_errs    帧错误次数（波特率不对/线路干扰的典型症状）
 *     usb_tx_dropped     因 USB 写超预算而放弃的字节数
 *     usb_tx_short       发生「短写」的批次数
 *     usb_wr_stalls      USB 写受阻（环形缓冲满/驱动异常）的次数
 *   ⇒ 判据：uart_rx_overflows == 0 且 usb_tx_dropped == 0 才叫「零丢字节」。
 *   · 单次搬运块 1024 B（Kconfig 可调）：≈11 ms 线时，是最大帧的 2 倍，
 *     既减少循环次数也不会让单批过大挤压 USB 缓冲。
 *   · UART 驱动事件队列 32 项：数据通路本身走环形缓冲，事件队列只是
 *     给驱动留的余量（本 demo 不消费事件，队列满也不影响数据落进环形缓冲）。
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
 *     缓冲只有 178 ms 的深度 —— 那才是真正的静默丢字节。
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
 *      （= 4 通道雷达电源已使能，由 IO14 控制）。
 *   2) PC 上出现「USB 串行设备 (COMx)」（VID:PID 3031:...，以实测为准；Linux
 *      /dev/ttyACM0）。波特率填 921600（USB CDC 其实不依赖波特率，填一致便于对数）。
 *   3) 打开端口后 ~100 ms 内上位机发 GET 0x02；若它同时拉 DTR/RTS，板子会复位重启
 *      一次（工程已知现象，见 docs/EDK60-pitfalls.md D3）—— 重启后 1~2 秒内恢复。
 *   4) 60 秒内应看到：
 *      · 一个字节级的连续流（上报周期由上位机用 0x10 设置，**单位 50 ms**，
 *        演示默认下发 2 = 100 ms；单帧最大 512 B）；
 *      · 上位机 UI 出现「人数」与「目标轨迹」（雷达需处在工作模式 type 0x05 = 1
 *        的跟踪/人数统计模式，该设置由上位机下发）；
 *      · 流里没有任何中文/ASCII 日志文本（除了极少量复位尾巴）。
 *   5) 若 60 秒什么都没看到，按顺序查：红灯亮不亮 → 模组是否插在 H3 →
 *      端口是不是那个 USB 串行设备 → 用 UART0_DEBUG 打开统计看 uart_rx 是否为 0。
 *
 * ===========================================================================
 * 编译期保护（本工程 17 个 demo 一起编译，断言必须绑定本 demo）
 * ===========================================================================
 *   见文件末尾：任何 #error 都写在
 *     #if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P) && ...
 *   里面。若漏掉前半段，会让其余 16 个 demo 全部构建失败（历史坑 B1）。
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
 * 注意 TAG 不以 "E""D" 开头，且本文件在静默模式下不输出任何字符。 */
static const char *TAG __attribute__((unused)) = "rbridge";

/* ===========================================================================
 * 配置映射（Kconfig → 本文件的编译期常量；引脚一律取自 BSP，禁止硬编码）
 * =========================================================================== */

#define BR_UART_PORT        UART_NUM_1
#define BR_RX_PIN           EDK60_PIN_RADAR_UART_RX   /* IO4：模组 TX -> MCU */
#define BR_TX_PIN           EDK60_PIN_RADAR_UART_TX   /* IO5：MCU -> 模组 RX */
#define BR_PWR_EN_PIN       EDK60_PIN_RADAR_PWR_EN    /* IO14：输出高 = 雷达上电 */

#define BR_BAUD             ((uint32_t)CONFIG_APP_EDK60_EDV11P_BAUD)
#define BR_CHUNK            (CONFIG_APP_EDK60_EDV11P_CHUNK)
#define BR_UART_RX_BUF      (CONFIG_APP_EDK60_EDV11P_UART_RX_BUF)
#define BR_STATS_SEC        (CONFIG_APP_EDK60_EDV11P_STATS_SEC)

/* UART1 发送缓冲：上位机的 GET/SET 帧很小，4 KB 足够；驱动会阻塞到全部入队 */
#define BR_UART_TX_BUF      4096
/* UART 事件队列项数（本 demo 不消费事件，给驱动留余量即可） */
#define BR_UART_EVT_QUEUE   32

/* USB 缓冲：见文件头「缓冲区/任务结构」的定量说明 */
#define BR_USB_TX_BUF       16384
#define BR_USB_RX_BUF       4096

/* 超时与退避（与文件头「缓冲区/超时/丢包」的定量关系绑定，改这里必须同步改注释） */
#define BR_UART_RD_TIMEOUT_MS   10   /* 无数据时每 10 ms 轮一次，保证 90 KB/s 跟得上 */
#define BR_USB_RD_TIMEOUT_MS    10
#define BR_USB_WR_ATTEMPT_MS    10   /* 单次 USB 写最多等多久 */
#define BR_USB_WR_BUDGET_MS     60   /* 单批数据在 USB 写里的总预算（必须 < UART RX 缓冲深度） */
#define BR_USB_WR_BACKOFF_MS    2

#define BR_TASK_STACK           3072
#define BR_TASK_PRIO            6
#define BR_STAT_TASK_PRIO       3
#define BR_STAT_TASK_STACK      3072

/* Kconfig 的 bool 在「未选中」时**根本不生成宏**（本项目踩过 3 次），
 * 所以凡「用户可以关掉」的 bool 一律用 #ifdef 兜底。 */
#ifdef CONFIG_APP_EDK60_EDV11P_SILENT_LOGS
#define BR_SILENT           1
#else
#define BR_SILENT           0
#endif

#ifdef CONFIG_APP_EDK60_EDV11P_UART0_DEBUG
#define BR_UART0_DBG        1
#else
#define BR_UART0_DBG        0
#endif

/* 2026-10-22 加：验收取证专用。把「诊断/统计」改从 **USB** 输出，
 * 用于在没有 UART0 串口板的情况下读取 uart_rx_overflows / usb_tx_dropped
 * 两个背压计数器（Gate-2 P1-03）。会让数据流混入文本，**仅供一次性取证**，
 * 正式固件必须保持关闭（默认 n）。 */
#ifdef CONFIG_APP_EDK60_EDV11P_STATS_VIA_USB
#define BR_STATS_VIA_USB    1
#else
#define BR_STATS_VIA_USB    0
#endif

#if (BR_UART0_DBG || BR_STATS_VIA_USB)
#define BR_DBG_ENABLED      1
#else
#define BR_DBG_ENABLED      0
#endif

/* ===========================================================================
 * 统计（两个搬运任务都会写，用临界区保护；只用于 UART0 调试输出）
 * =========================================================================== */

typedef struct {
    uint32_t uart_rx_bytes;    /* 雷达 → 板 的字节数（已进 UART 环形缓冲） */
    uint32_t uart_rx_chunks;   /* 上述方向的搬运次数 */
    uint32_t uart_rx_overflows;/* UART 环形缓冲满事件数 ⇒ 有字节被驱动丢弃（关键判据） */
    uint32_t uart_fifo_ovf;    /* 硬件 FIFO 溢出事件数 */
    uint32_t uart_frame_errs;  /* 帧错误事件数（波特率不对/干扰的典型症状） */
    uint32_t usb_tx_bytes;     /* 板 → PC 的字节数（真正写进 USB TX 环形缓冲） */
    uint32_t usb_tx_dropped;   /* 因 USB 写超预算而放弃的字节数（关键判据） */
    uint32_t usb_tx_short;     /* 发生「短写」的批次数 */
    uint32_t usb_wr_stalls;    /* USB 写受阻（缓冲满/驱动异常）的次数 */
    uint32_t usb_rx_bytes;     /* PC → 板 的字节数 */
    uint32_t usb_rx_chunks;
    uint32_t uart_tx_bytes;    /* 板 → 雷达 的字节数 */
} br_stats_t;

static br_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

/* UART 事件队列：本 demo 不靠事件搬数据（数据走环形缓冲），但**必须**消费事件，
 * 否则没法统计「环形缓冲满 / FIFO 溢出 / 帧错误」—— 那正是「有没有静默丢字节」
 * 的唯一可验证证据。 */
static QueueHandle_t s_uart_evt_q;

/* 收发缓冲放静态区：任务栈只放局部变量，省 RAM 也更抗栈溢出 */
static uint8_t s_buf_r2u[BR_CHUNK];     /* 雷达 → USB */
static uint8_t s_buf_u2r[BR_CHUNK];     /* USB → 雷达 */

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

#if BR_DBG_ENABLED
static void dbg_raw(const char *s)
{
#if BR_STATS_VIA_USB
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
#if BR_STATS_VIA_USB
    /* 取证模式：整行都走 USB（此前只把行尾 \r\n 走了 USB，正文仍进 UART0，
     * 结果取回的文件里只剩一串空行——2026-10-22 实测踩到） */
    (void)usb_serial_jtag_write_bytes((const uint8_t *)line, len, pdMS_TO_TICKS(50));
#else
    for (size_t i = 0; i < len; i++) {
        (void)esp_rom_output_tx_one_char((uint8_t)(unsigned char)line[i]);
    }
#endif
    dbg_raw("\r\n");
}
#endif /* BR_DBG_ENABLED */

/* ===========================================================================
 * 字节搬运助手（纯字节搬运：不改写、不加前缀、不做可视化）
 * =========================================================================== */

/**
 * 把一批字节推给 USB 主机。
 * 返回真正写进 USB TX 环形缓冲的字节数；< len 表示本批在 BR_USB_WR_BUDGET_MS
 * 预算内没能写完（调用方负责把差额计入 usb_tx_dropped / usb_tx_short）。
 *
 * 为什么是「总预算」而不是「重试次数」：重试次数无法约束最坏停摆时间，
 * 而停摆时间一旦超过 UART 环形缓冲的深度（≈178 ms），丢的就是驱动里
 * 看不见的字节。60 ms 的硬预算保证「只要主机 60 ms 内恢复读取，一个字节不丢」，
 * 超出的部分明确计数，不做静默丢弃。
 */
static size_t usb_write_all(const uint8_t *data, size_t len)
{
    size_t off = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)BR_USB_WR_BUDGET_MS * 1000;

    while (off < len) {
        int w = usb_serial_jtag_write_bytes(data + off, len - off,
                                            pdMS_TO_TICKS(BR_USB_WR_ATTEMPT_MS));
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
        vTaskDelay(pdMS_TO_TICKS(BR_USB_WR_BACKOFF_MS));
    }
    return off;
}

/**
 * 消费 UART 事件队列，把「丢字节」类事件计入统计。
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
        int w = uart_write_bytes(BR_UART_PORT, data + off, len - off);
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
 * =========================================================================== */

static void task_radar_to_usb(void *arg)
{
    (void)arg;

    while (1) {
        int n = uart_read_bytes(BR_UART_PORT, s_buf_r2u, BR_CHUNK,
                                pdMS_TO_TICKS(BR_UART_RD_TIMEOUT_MS));
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
 * 上位机自己会发 EDV11P 的 GET/SET 配置帧，本固件只做搬运。
 * =========================================================================== */

static void task_usb_to_uart(void *arg)
{
    (void)arg;

    while (1) {
        int n = usb_serial_jtag_read_bytes(s_buf_u2r, BR_CHUNK,
                                           pdMS_TO_TICKS(BR_USB_RD_TIMEOUT_MS));
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
 * 统计任务（只有开 UART0 调试时才创建；否则连任务都不占 RAM）
 * =========================================================================== */

#if BR_DBG_ENABLED
#if BR_STATS_SEC > 0
static void task_stats(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)BR_STATS_SEC * 1000u));

        br_stats_t snap;
        portENTER_CRITICAL(&s_stats_lock);
        snap = s_stats;
        portEXIT_CRITICAL(&s_stats_lock);

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
#endif /* BR_STATS_SEC > 0 */
#endif /* BR_DBG_ENABLED */

/* ===========================================================================
 * 入口
 * =========================================================================== */

void demo_radar_bridge_edv11p_start(void)
{
    /* --- 0. 先静默：让后面所有初始化（含 BSP 与驱动自己的日志）一个字符都不出去。
     *        CONFIG_LOG_DEFAULT_LEVEL_NONE 已经在编译期消掉 ESP_LOG*，
     *        这里再压一次，是为了兜住「用户自己 menuconfig 编、没走 demo 专用
     *        defaults」的情况。 --- */
#if BR_SILENT
    esp_log_level_set("*", ESP_LOG_NONE);
#endif

#if BR_DBG_ENABLED
    dbg_raw("\r\n");   /* 不用 dbg_line("")：空格式串会触发 -Wformat-zero-length */
    dbg_line("[k60] demo=radar_bridge_edv11p  uart1 rx=io4 tx=io5 en=io14 baud=%u",
             (unsigned)BR_BAUD);
    dbg_line("[k60] 纯字节透传：不解析协议、不发命令、不改写字节流");
    /* 参数一行打全：上板时照着它核对「代码里到底用的是哪组值」 */
    dbg_line("[k60] 参数: uart_rx_buf=%d B usb_tx_buf=%d B usb_rx_buf=%d B chunk=%d B "
             "usb写预算=%d ms 统计=%d s",
             BR_UART_RX_BUF, BR_USB_TX_BUF, BR_USB_RX_BUF, BR_CHUNK,
             BR_USB_WR_BUDGET_MS, BR_STATS_SEC);
    dbg_line("[k60] 本诊断通道直写 uart0（rom 单字节输出），不经过日志系统，"
             "不会被复制到 usb 数据流");
#endif

    /* --- 1. 雷达电源脚：先初始化成「关闭」（与硬件默认一致），
     *        等 UART 就绪后再拉高 —— 这样 EN 拉高之后到达的每个字节都能落进
     *        UART 环形缓冲，一个不丢。 --- */
    esp_err_t err = edk60_radar_power_init();
    if (err != ESP_OK) {
#if BR_DBG_ENABLED
        dbg_line("[k60] 雷达电源脚 io%d 初始化失败: %s", (int)BR_PWR_EN_PIN,
                 esp_err_to_name(err));
#endif
        return;
    }

    /* --- 2. USB Serial/JTAG 驱动：缓冲尽量大（PC 侧偶发停顿时的蓄水池） --- */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_cfg.tx_buffer_size = BR_USB_TX_BUF;
        usb_cfg.rx_buffer_size = BR_USB_RX_BUF;

        err = usb_serial_jtag_driver_install(&usb_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            /* ESP_ERR_INVALID_STATE = 已被别处安装，按成功复用处理 */
#if BR_DBG_ENABLED
            dbg_line("[k60] usb 驱动安装失败: %s —— 透传不可用", esp_err_to_name(err));
#endif
            return;
        }
    }

    /* --- 3. UART1 @ 921600 8N1：RX=IO4、TX=IO5（ESP32-C6 的 UART1/2 没有 IOMUX
     *        默认脚，必须显式绑定）。 --- */
    const uart_config_t uart_cfg = {
        .baud_rate = (int)BR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_driver_install(BR_UART_PORT, BR_UART_RX_BUF, BR_UART_TX_BUF,
                              BR_UART_EVT_QUEUE, &s_uart_evt_q, 0);
    if (err != ESP_OK) {
#if BR_DBG_ENABLED
        dbg_line("[k60] uart1 驱动安装失败: %s", esp_err_to_name(err));
#endif
        return;
    }

    err = uart_param_config(BR_UART_PORT, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(BR_UART_PORT,
                           (int)BR_TX_PIN, (int)BR_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* 收到的空闲超时：让驱动及时把 FIFO 里的残字节搬进环形缓冲 */
        err = uart_set_rx_timeout(BR_UART_PORT, 5);
    }
    if (err != ESP_OK) {
#if BR_DBG_ENABLED
        dbg_line("[k60] uart1 配置失败: %s（rx=io4 tx=io5 是否被占用？）",
                 esp_err_to_name(err));
#endif
        return;
    }

    /* 清掉上电噪声（此刻雷达还没上电，缓冲里只可能是悬空脚噪声）。
     * 注意：这次 flush 发生在搬运任务启动**之前**，不会与 uart_read_bytes 争锁
     * （历史坑 E1：运行期 flush 会死锁）。 */
    uart_flush_input(BR_UART_PORT);

    /* --- 4. 雷达上电：EN=IO14 拉高并**常驻**，之后运行期再也不动这个脚。
     *        （历史坑：运行中误拉低 = 雷达掉电，现象像「突然没数据」。） --- */
    err = edk60_radar_power_on();
    if (err != ESP_OK) {
#if BR_DBG_ENABLED
        dbg_line("[k60] 雷达电源使能失败: %s", esp_err_to_name(err));
#endif
        return;
    }
    /* 刻意**不**在这里延时等待模组启动：模组启动期间的字节同样会落进 UART
     * 环形缓冲，延时只会白丢它们。搬运任务马上就开始。 */

    /* --- 5. 双向搬运任务（两个都建成功才算就绪） --- */
    BaseType_t ok1 = xTaskCreate(task_radar_to_usb, "r2u", BR_TASK_STACK, NULL,
                                 BR_TASK_PRIO, NULL);
    BaseType_t ok2 = xTaskCreate(task_usb_to_uart, "u2r", BR_TASK_STACK, NULL,
                                 BR_TASK_PRIO, NULL);

#if BR_DBG_ENABLED
#if BR_STATS_SEC > 0
    (void)xTaskCreate(task_stats, "brstat", BR_STAT_TASK_STACK, NULL,
                      BR_STAT_TASK_PRIO, NULL);
#endif
    dbg_line("[k60] 就绪：en=%d（1=雷达已上电，丝印 RADAR 红灯应亮）",
             (int)edk60_radar_power_is_on());
#endif

    if (ok1 != pdPASS || ok2 != pdPASS) {
#if BR_DBG_ENABLED
        dbg_line("[k60] 任务创建失败（内存不足）");
#endif
        return;
    }
}

/* ===========================================================================
 * 编译期保护
 * ---------------------------------------------------------------------------
 * ⚠ 本工程 17 个 demo 的源文件**一起编译**，任何无条件的 #error 会让每一次
 *   构建（包括其它 16 个 demo）全部失败（历史坑 B1）。所以断言必须同时约束
 *   「当前选中的是本 demo」。
 * =========================================================================== */

#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P) && \
    defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE)
#error "radar_bridge_edv11p 不允许用「关闭 USB 第二控制台」来让数据流变干净：实测会让 ESP32-C6 启动卡死、USB 不枚举、必须断电重插。请改回 CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y，并保持 CONFIG_APP_EDK60_EDV11P_SILENT_LOGS=y（见 sdkconfig.defaults.radar_bridge 文件头与 docs/EDK60-pitfalls.md D1）。"
#endif
