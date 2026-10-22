/*
 * EDK60 Demo 5.1 —— radar_bridge（雷达透传桥）
 *
 * ==========================================================================
 * 目的
 * ==========================================================================
 * 把插在 H4 上的雷达模组（如 EDV151）的串口数据，**原样字节搬运**到
 * ESP32-C6 的原生 USB 串口，让 PC 上的上位机直接收数据：
 *
 *     雷达模组 ──UART──► ESP32-C6 ──USB Serial/JTAG──► PC 上位机(COMx)
 *        (H4-T)          (IO12/IO13，USB-C 口)
 *
 * 本 demo **不解析任何雷达协议**：不认识帧头、不做校验、不做单位换算，
 * 进来什么字节就吐什么字节。因此它适用于任何「UART 输出的雷达模组」。
 *
 * 如果要把 ed_radar 组件的协议解析也接上（目标存在/距离/跌倒等结构化数据），
 * 那是另一类 demo 的职责；本 demo 只负责把原始字节流交给上位机。
 *
 * ==========================================================================
 * 硬件依据（原理图 docs/SCH_EDK60_VA.pdf，EDK60_VA V1.0）
 * ==========================================================================
 *   · H4 为雷达模组插拔座（PM200-1-08-Z-3.0-C）：
 *         T = 模组 TX → MCU（本 demo 的 RX 源，默认按 IO14 处理）
 *         R = RADAR_PWR_EN（U4 的 EN，10k 下拉到 GND，输出高才使能）
 *         P/V = 3.3V_RADAR  G = GND   A = IO2/IO3/IO4/IO5（模拟量款用）
 *   · USB-C 的 D-/D+ 直连 C6 的原生 USB Serial/JTAG（IO12/IO13），
 *     板上没有 CH340/CP2102 —— 「USB 转串口」由芯片自己完成。
 *   · ⚠ **已知读图歧义**：原理图在 U4.EN 走线上同时标了 RADAR_PWR_EN 与 IO14，
 *     因此「模组 TX 到底进 IO14 还是 IO15」无法仅凭图纸唯一确定。
 *     本 demo 用「自动识别」把这件事交给固件试出来，见下面的 §自动识别。
 *   · ⚠ 本板 H4 **没有引出 MCU→模组 的独立数据线**（IO15 被电源使能占用），
 *     所以默认只有「雷达 → PC」一个方向。若你改板引出空闲 GPIO，
 *     把 APP_EDK60_BRIDGE2_TX_PIN 填上即可打通反向。
 *
 * ==========================================================================
 * 控制台与数据的分工（重要 —— 这是本 demo 最容易踩的坑）
 * ==========================================================================
 *   日志（ESP_LOG / printf）→ 主控制台 UART0（H2 的 D1/TX=IO17，115200）
 *   雷达数据                → USB Serial/JTAG（USB-C 口，PC 的上位机）
 *
 * 为什么要这样分：ESP32-C6 的 USB Serial/JTAG 只有**一个**（IO12/IO13），
 * 在 PC 上就是一个 USB 串行设备(COMx)。若把控制台也接到 USB（即
 * CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y），日志会与雷达数据
 * 写进同一个 FIFO，上位机看到的就是「数据夹着中文日志」的混合流。
 * 因此本 demo：
 *   · 用 sdkconfig.defaults.radar_bridge 关掉 USB 第二输出口；
 *   · 并在文件顶部用 #error 做编译期保护，防止被改回去。
 * 结果：USB 上跑的是**干净的数据**，只有真正的调试场景才需要
 * 打开 APP_EDK60_BRIDGE2_UNMUTE_LOGS（那时日志会插入数据流，仅限排查）。
 *
 * 补充：JTAG 不会干扰 —— 它是 USB Serial/JTAG 的分时复用功能，
 * 不启动调试器时不占用链路；只有「第二控制台输出」才是污染源。
 *
 * ==========================================================================
 * 自动识别（APP_EDK60_BRIDGE2_AUTODETECT，默认开）
 * ==========================================================================
 *   步骤 1 —— 找 RX 脚：
 *     把候选引脚（默认 14,15,2,3,4,5）依次配成 UART 的 RX（其它保持高阻输入），
 *     每个脚监听 PROBE_MS 毫秒，统计收到的字节数，取最多者。
 *     为什么安全：只把脚配成输入，不会驱动外部电路；即便候选里包含了
 *     电源使能脚，也不会与外部 10k 下拉打架。
 *   步骤 2 —— 找波特率：
 *     在识别到的脚上，依次试候选波特率（默认 921600,115200,256000,...），
 *     统计字节数与「字节种类数」，给出评分并取最优。
 *     注意 ESP32-C6 **没有**硬件波特率自动检测（那是老 ESP32 才有的
 *     uart_baudrate_detect），所以这里是软件穷举 + 启发式评分：
 *       · 波特率对了，单位时间收到的字节数明显更多、字节种类更丰富；
 *       · 波特率错了，通常只会收到少量、单调的乱码。
 *     因此识别结果是**"最像正常串流的那一组"，不是绝对保证** ——
 *     日志会同时打印每个候选的得分，你可以据此人工确认。
 *   步骤 3 —— 若所有候选脚都收不到任何字节：
 *     打印排查清单（模组是否上电/是否插好/T、R 是否接反/波特率不在候选列表里），
 *     并提示用万用表量 H4 的 P/V 脚确认 3.3V_RADAR —— 这一步同时能把
 *     上面那条 IO14/IO15 歧义一并定论。
 *
 * ==========================================================================
 * 观察点（上板怎么确认它在工作）
 * ==========================================================================
 *   1) 串口终端（接 H2 的 D0/D1，115200）能看到本 demo 的启动横幅与识别结果；
 *   2) 雷达使能后，丝印 "L" 的橙色 LED4 应点亮（它由使能脚驱动）；
 *   3) PC 上打开 USB 对应的串口（设备管理器里的「USB 串行设备」），
 *      波特率任意（USB CDC 不受波特率限制），应能看到持续刷新的原始字节；
 *   4) 在 USB 串口里发几个字符 —— 若本地回环自测开着，会原样回显，
 *      用来证明「上位机 → 板子」这条通路也是通的；
 *   5) 控制台每 STATS_SEC 秒打印一次双向字节数与包数，用于确认链路上有流量。
 *
 * ==========================================================================
 * 安全说明
 * ==========================================================================
 *   · 本 demo 不做任何擦除/删除动作；不写 NVS；不修改分区。
 *   · 退出时（如果有）会释放 UART 与 USB 驱动，不影响下次启动。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

static const char *TAG = "radar_bridge";

/* ==========================================================================
 * 关于「日志会不会污染 USB 数据流」—— 实测结论，别再用旧方案
 * --------------------------------------------------------------------------
 * 事实：ESP32-C6 的 USB Serial/JTAG 只有**一个**外设（IO12/IO13），
 * 在 PC 上就是一个「USB 串行设备」。所以只要日志被送到 USB，就会和雷达数据
 * 写进同一个 FIFO，上位机收到「数据夹日志」的混合流。
 *
 * ❌ 曾经试过的方案（**2026-10-15 上板实测证伪，不要再用**）：
 *      CONFIG_ESP_CONSOLE_SECONDARY_NONE=y     ← 关掉 USB 第二控制台
 *    · 结果：固件启动**直接卡死**在 `sleep_gpio` 之后，连 `main_task: Started`
 *      都不打印；USB 口不再枚举，必须物理断电重插才能恢复。
 *    · 对照实验：同一块板烧 hello_board（该选项保持开启）完全正常启动。
 *    · 结论：C6 上把两个控制台都从 USB 撤掉会让系统初始化挂死。
 *
 * ✅ 现在采用的方案（见 sdkconfig.defaults.radar_bridge）：
 *    · 控制台配置**保持工程默认**（UART0 主控制台 + USB 第二输出口），
 *      启动路径与其它 demo 完全一致 → 不会卡死；
 *    · CONFIG_LOG_DEFAULT_LEVEL_NONE=y → ESP_LOG* / printf 不产生任何字符，
 *      于是 USB 上天然没有日志，只剩雷达数据。
 *
 * 所以本文件**不需要**也**不应该**再加「检测到 USB 控制台就报错」这类断言：
 * USB 控制台开着才是正确状态，静音靠日志级别实现。
 * ========================================================================== */

/* ---------------------------------------------------------------------------
 * 配置映射（把 Kconfig 的值收拢到这里，方便阅读与排错）
 * ------------------------------------------------------------------------- */

#define BRIDGE_UART_PORT        ((uart_port_t)CONFIG_APP_EDK60_BRIDGE2_UART_NUM)
#define BRIDGE_UART_PORT_NAME   (CONFIG_APP_EDK60_BRIDGE2_UART_NUM == 1 ? "UART1" : "UART2")
#define BRIDGE_RX_PIN           (CONFIG_APP_EDK60_BRIDGE2_RX_PIN)
#define BRIDGE_TX_PIN           (CONFIG_APP_EDK60_BRIDGE2_TX_PIN)
/* TX 脚 >= 0 才表示我们能与模组通信（用于开机 turn_on） */
#if CONFIG_APP_EDK60_BRIDGE2_TX_PIN >= 0
#define BRIDGE_HAS_RADAR_TX     1
#else
#define BRIDGE_HAS_RADAR_TX     0
#endif

#define BRIDGE_BAUD             ((uint32_t)CONFIG_APP_EDK60_BRIDGE2_BAUD)
/* Kconfig 里 default n 的 bool 不会生成 CONFIG_* 宏，必须用 #ifdef 兜底 */
#ifdef CONFIG_APP_EDK60_BRIDGE2_AUTODETECT
#define BRIDGE_AUTODETECT       1
#else
#define BRIDGE_AUTODETECT       0
#endif
#define BRIDGE_PROBE_MS         (CONFIG_APP_EDK60_BRIDGE2_PROBE_MS)

#define BRIDGE_CHUNK            (CONFIG_APP_EDK60_BRIDGE2_CHUNK)
#define BRIDGE_STATS_SEC        (CONFIG_APP_EDK60_BRIDGE2_STATS_SEC)

/* Kconfig 里 default y 但用户可关掉的 bool：关掉后**不生成宏**，
 * 所以必须 #ifdef 兜底，不能直接引用 CONFIG_（本项目已多次踩到）*/
#ifdef CONFIG_APP_EDK60_BRIDGE2_LOCAL_ECHO
#define BRIDGE_LOCAL_ECHO       1
#else
#define BRIDGE_LOCAL_ECHO       0
#endif
/*
 * 注意：`default n` 的 bool 配置在 sdkconfig.h 里**根本不会生成宏**，
 * 所以这里必须用 #ifdef 兜底，不能直接写 CONFIG_APP_EDK60_BRIDGE2_LOCAL_ECHO_PREFIX，
 * 否则一旦该选项关闭就是「未声明标识符」编译错误。
 */
#ifdef CONFIG_APP_EDK60_BRIDGE2_LOCAL_ECHO_PREFIX
#define BRIDGE_ECHO_PREFIX      1
#else
#define BRIDGE_ECHO_PREFIX      0
#endif

/* 注意宏名必须与 Kconfig 完全一致：config 名是 APP_EDK60_BRIDGE2_PWR_EN_IO15
 * （前面是 choice 名 APP_EDK60_BRIDGE2_PWR_EN_PIN，但 config 名里没有 _PIN_）。
 * 这里曾经写成 CONFIG_..._PWR_EN_PIN_IO15 导致判断永远为假 —— 雷达电源从未被使能，
 * 上板表现为「6 个候选脚全是 0 字节」，已修正。 */
#if CONFIG_APP_EDK60_BRIDGE2_PWR_EN_IO15
#define BRIDGE_HAS_PWR_EN       1
#define BRIDGE_PWR_EN_PIN       EDK60_PIN_RADAR_PWR_EN
#else
#define BRIDGE_HAS_PWR_EN       0
#define BRIDGE_PWR_EN_PIN       (-1)
#endif

/* UART 缓冲区：RX 要比单帧大得多，避免 921600bps 下丢数据 */
#define BRIDGE_UART_RX_BUF      8192
#define BRIDGE_UART_TX_BUF      2048
/* USB 缓冲区 */
#define BRIDGE_USB_RX_BUF       4096
#define BRIDGE_USB_TX_BUF       8192
/* 单次读超时 */
#define BRIDGE_RD_TIMEOUT_MS    50

/* 自动识别用：候选列表最多支持多少项 */
#define MAX_CANDIDATES          12

/* ---------------------------------------------------------------------------
 * 统计（多任务访问，用临界区保护即可）
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t radar_to_usb_bytes;   /* 雷达 → USB 的字节数 */
    uint32_t radar_to_usb_chunks;  /* 上述方向的搬运次数 */
    uint32_t usb_to_radar_bytes;   /* USB → 雷达 的字节数 */
    uint32_t usb_to_radar_chunks;  /* 上述方向的搬运次数 */
    uint32_t echo_bytes;           /* 本地回环自测回显的字节数 */
    uint32_t usb_to_radar_dropped; /* 因未配置 TX 脚而丢弃的字节数 */
} bridge_stats_t;

static bridge_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_usb_tx_lock;   /* USB 写做互斥，避免两条流交叉撕裂 */

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

static inline void stats_add(uint32_t *field, uint32_t n)
{
    portENTER_CRITICAL(&s_stats_lock);
    *field += n;
    portEXIT_CRITICAL(&s_stats_lock);
}

static inline uint32_t stats_get(uint32_t *field)
{
    portENTER_CRITICAL(&s_stats_lock);
    uint32_t v = *field;
    portEXIT_CRITICAL(&s_stats_lock);
    return v;
}

/** 写 USB（加锁；对端没打开时 write 会失败/超时，只告警一次不刷屏） */
static bool usb_write(const uint8_t *data, uint32_t len)
{
    if (len == 0) {
        return true;
    }
    bool wrote = false;
    if (s_usb_tx_lock && xSemaphoreTake(s_usb_tx_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        int n = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(200));
        wrote = (n == (int)len);
        xSemaphoreGive(s_usb_tx_lock);
    } else if (!s_usb_tx_lock) {
        int n = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(200));
        wrote = (n == (int)len);
    }
    return wrote;
}

/** 把逗号分隔的十进制列表解析成数组，返回元素个数（自动去重、去空白） */
static int __attribute__((unused)) parse_csv_ints(const char *str, int *out, int out_cap)
{
    if (!str || !out || out_cap <= 0) {
        return 0;
    }
    /* strtok 需要可写 buffer */
    static char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok != NULL && n < out_cap;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        char *end = NULL;
        long v = strtol(tok, &end, 10);
        if (end == tok) {
            continue;   /* 非数字，跳过 */
        }
        bool dup = false;
        for (int i = 0; i < n; i++) {
            if (out[i] == (int)v) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out[n++] = (int)v;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * UART 配置
 * ------------------------------------------------------------------------- */

static esp_err_t bridge_uart_install(void)
{
    const uart_config_t cfg = {
        .baud_rate = BRIDGE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BRIDGE_UART_PORT, BRIDGE_UART_RX_BUF, BRIDGE_UART_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 驱动安装失败: %s", BRIDGE_UART_PORT_NAME, esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_PORT, &cfg));

    /* ESP32-C6 的 UART1/UART2 没有 IOMUX 默认引脚，必须显式绑定。
     * TX 为 -1 时用 UART_PIN_NO_CHANGE：不占任何引脚（纯接收）。 */
    err = uart_set_pin(BRIDGE_UART_PORT,
                       (BRIDGE_TX_PIN >= 0) ? BRIDGE_TX_PIN : UART_PIN_NO_CHANGE,
                       (BRIDGE_RX_PIN >= 0) ? BRIDGE_RX_PIN : UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin 失败: %s（TX=%d RX=%d）", esp_err_to_name(err),
                 BRIDGE_TX_PIN, BRIDGE_RX_PIN);
        return err;
    }
    ESP_ERROR_CHECK(uart_set_rx_timeout(BRIDGE_UART_PORT, 5));

    /* 清掉上电噪声 */
    uart_flush_input(BRIDGE_UART_PORT);
    vTaskDelay(pdMS_TO_TICKS(20));
    uart_flush_input(BRIDGE_UART_PORT);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) bridge_uart_set_rx_pin(int pin)
{
    return uart_set_pin(BRIDGE_UART_PORT,
                        (BRIDGE_TX_PIN >= 0) ? BRIDGE_TX_PIN : UART_PIN_NO_CHANGE,
                        pin,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static esp_err_t __attribute__((unused)) bridge_uart_set_baud(uint32_t baud)
{
    esp_err_t err = uart_set_baudrate(BRIDGE_UART_PORT, baud);
    if (err == ESP_OK) {
        uart_flush_input(BRIDGE_UART_PORT);
    }
    return err;
}

/* ---------------------------------------------------------------------------
 * USB Serial/JTAG
 * ------------------------------------------------------------------------- */

static esp_err_t bridge_usb_install(void)
{
    /* USB 写需要一把锁：雷达数据流与本地回环/日志都往 USB 写，不加锁会互相撕裂 */
    if (s_usb_tx_lock == NULL) {
        s_usb_tx_lock = xSemaphoreCreateMutex();
        if (s_usb_tx_lock == NULL) {
            ESP_LOGE(TAG, "创建 USB 写锁失败（内存不足），将无锁写入（可能出现字节交叉）");
        }
    }

    if (usb_serial_jtag_is_driver_installed()) {
        ESP_LOGI(TAG, "USB Serial/JTAG 驱动已安装（可能是控制台主输出口），直接复用");
        return ESP_OK;
    }
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = BRIDGE_USB_TX_BUF,
        .rx_buffer_size = BRIDGE_USB_RX_BUF,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB Serial/JTAG 已被占用（%s），按已安装处理", esp_err_to_name(err));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB Serial/JTAG 驱动已安装（IO12=D-, IO13=D+, TXbuf=%d RXbuf=%d）",
             BRIDGE_USB_TX_BUF, BRIDGE_USB_RX_BUF);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * 雷达电源
 * ------------------------------------------------------------------------- */

static void radar_power_setup(void)
{
    if (!BRIDGE_HAS_PWR_EN) {
        ESP_LOGW(TAG, "按配置**不占用**电源使能脚 —— 雷达电源仍由硬件默认状态决定");
        ESP_LOGW(TAG, "（U4 的 EN 经 10k 下拉到 GND，默认是**关闭**的；"
                      "若模组不上电，请改用「占用 IO%d」的配置）", EDK60_PIN_RADAR_PWR_EN);
        return;
    }

    ESP_ERROR_CHECK(edk60_radar_power_init());   /* 推挽输出 + 置为关闭 */

#if CONFIG_APP_EDK60_BRIDGE2_PWR_ON_AT_BOOT
    ESP_LOGI(TAG, "使能雷达电源（IO%d 输出高 → EN=1），随后等待 %d ms 让模组启动",
             BRIDGE_PWR_EN_PIN, CONFIG_APP_EDK60_BRIDGE2_PWR_READY_MS);
    ESP_ERROR_CHECK(edk60_radar_power_on());
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_EDK60_BRIDGE2_PWR_READY_MS));
    ESP_LOGI(TAG, "雷达电源状态：EN=%s（丝印 \"L\" 的橙色 LED4 应点亮；"
                  "丝印 \"RADAR\" 的红灯应同步亮起）",
             edk60_radar_power_is_on() ? "高(已使能)" : "低(仍关闭)");
#else
    ESP_LOGI(TAG, "按配置未在启动时使能雷达电源（保持硬件默认关闭）");
#endif
}

/* ---------------------------------------------------------------------------
 * 自动识别：找 RX 脚 + 找波特率
 * ------------------------------------------------------------------------- */

typedef struct {
    int pin;
    uint32_t bytes;
    int distinct;
} pin_probe_t;

typedef struct {
    uint32_t baud;
    uint32_t bytes;
    int distinct;
} baud_probe_t;

/** 在给定波特率下监听一段时间的字节数（含 IO14~IO21 的其它候选，用输入模式试） */
static void __attribute__((unused)) probe_bytes(uint32_t *out_bytes, int *out_distinct)
{
    static uint8_t buf[256];
    uint8_t seen[256];
    memset(seen, 0, sizeof(seen));

    uint32_t total = 0;
    int distinct = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)BRIDGE_PROBE_MS * 1000;

    while (esp_timer_get_time() < deadline) {
        int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            if (seen[buf[i]] == 0) {
                seen[buf[i]] = 1;
                distinct++;
            }
        }
        total += (n > 0) ? (uint32_t)n : 0;
    }

    *out_bytes = total;
    *out_distinct = distinct;
}

#if BRIDGE_AUTODETECT

/**
 * 步骤 1：找出哪根候选脚在「有数据」。
 * 返回该脚号；若全部无数据返回 -1。
 */
static int autodetect_rx_pin(void)
{
    int pins[MAX_CANDIDATES];
    int n = parse_csv_ints(CONFIG_APP_EDK60_BRIDGE2_RX_PIN_LIST, pins, MAX_CANDIDATES);
    if (n <= 0) {
        ESP_LOGW(TAG, "候选引脚列表为空或格式错误（当前=\"%s\"），跳过引脚识别",
                 CONFIG_APP_EDK60_BRIDGE2_RX_PIN_LIST);
        return -1;
    }

    /* ⚠ 探测期间**必须保持雷达电源使能**，绝不能把使能脚释放成高阻输入。
     * 曾经这么写过，结果是：一释放 EN 就把 3.3V_RADAR 关掉 ⇒ 模组断电 ⇒
     * 6 个候选脚全部 0 字节，看起来像"模组不输出"，实际是自己把电源关了。
     * 正确做法：让 EN 一直输出高，并把 EN 脚本身从候选列表里剔除
     * （见下面的跳过逻辑），因为扫描它同样等于跟电源使能抢这根线。 */
    if (BRIDGE_HAS_PWR_EN) {
        edk60_radar_power_on();
    }

    ESP_LOGI(TAG, "===== 步骤 1/2：识别接收脚（每个候选监听 %d ms，波特率暂用 %u）=====",
             BRIDGE_PROBE_MS, (unsigned)BRIDGE_BAUD);
    ESP_LOGI(TAG, "候选脚：%s", CONFIG_APP_EDK60_BRIDGE2_RX_PIN_LIST);
    ESP_LOGI(TAG, "提示：只把这些脚配成 UART 的 RX（输入），不会驱动外部电路，可安全扫描。");

    pin_probe_t best = { .pin = -1, .bytes = 0, .distinct = 0 };

    for (int i = 0; i < n; i++) {
        int pin = pins[i];
        if (pin < 0 || pin > 23) {
            continue;
        }
#if BRIDGE_HAS_PWR_EN
        if (pin == BRIDGE_PWR_EN_PIN) {
            ESP_LOGW(TAG, "  IO%-2d 跳过：这是雷达电源使能脚，当 RX 用会让雷达掉电"
                          "（如怀疑数据线在这根，请改配置把电源使能设为『不占用』再试）", pin);
            continue;
        }
#endif
        if (bridge_uart_set_rx_pin(pin) != ESP_OK) {
            ESP_LOGW(TAG, "  IO%-2d 无法配成 RX，跳过", pin);
            continue;
        }
        /* 等一小会儿让线稳定，再清空缓冲，保证统计只反映本次窗口 */
        vTaskDelay(pdMS_TO_TICKS(20));
        uart_flush_input(BRIDGE_UART_PORT);

        uint32_t bytes = 0;
        int distinct = 0;
        probe_bytes(&bytes, &distinct);

        /* 用 IO 号顺便标出它在板上是什么（便于对图） */
        const char *note = "";
        if (pin == EDK60_PIN_RADAR_UART_RX) {
            note = "  <- H4 的 T 脚（图纸标 IO14）";
        } else if (pin == EDK60_PIN_RADAR_PWR_EN) {
            note = "  <- H4 的 R 脚 / RADAR_PWR_EN";
        } else if (pin >= 2 && pin <= 5) {
            note = "  <- H4 的 A 脚之一";
        } else if (pin == 0) {
            note = "  <- H3 的 A 脚（插在 H3 时看这里）";
        } else if (pin == 1) {
            note = "  <- H3 的 R 脚（插在 H3 时看这里）";
        }

        ESP_LOGI(TAG, "  IO%-2d : 收到 %6u 字节 / %3d 种字节%s",
                 pin, (unsigned)bytes, distinct, note);

        if (bytes > best.bytes) {
            best.pin = pin;
            best.bytes = bytes;
            best.distinct = distinct;
        }
    }

    /* 电源使能脚全程保持使能，这里只需再确认一次（防止中途被别的配置改动） */
    if (BRIDGE_HAS_PWR_EN) {
        edk60_radar_power_on();
    }

    if (best.pin < 0 || best.bytes == 0) {
        ESP_LOGW(TAG, "===== 所有候选脚都没有收到任何字节 =====");
        ESP_LOGW(TAG, "请按顺序排查：");
        ESP_LOGW(TAG, "  1) 模组是否已插在 H4 上、方向正确；");
        ESP_LOGW(TAG, "  2) 模组是否上电 —— 丝印 \"L\" 的橙色 LED4 应亮；"
                      "不亮就用万用表量 H4 的 P/V 脚对 GND 是否有 3.3V；");
        ESP_LOGW(TAG, "  3) 3.3V_RADAR 有 3.3V 但 LED4 不亮 ⇒ 说明使能脚不是 IO%d，"
                      "请把「雷达 RX 引脚」改成 IO%d 再试（这一步同时能定论图纸上 "
                      "IO14/IO15 的归属歧义）", BRIDGE_PWR_EN_PIN, EDK60_PIN_RADAR_PWR_EN);
        ESP_LOGW(TAG, "  4) H4 的 T 与 R 是否被模组侧接反（模组 TX 应进 MCU）；");
        ESP_LOGW(TAG, "  5) 模组波特率不在候选列表里 —— 把你的手册值填进 "
                      "\"自动识别的候选波特率\" 或直接设为固定波特率；");
        ESP_LOGW(TAG, "  6) 部分模组需要先收到配置命令才开始输出 —— 本板没有 MCU→模组"
                      " 的数据线，这种情况需改板。");
        return -1;
    }

    ESP_LOGI(TAG, "==> 接收脚识别结果：IO%d（%u 字节）", best.pin, (unsigned)best.bytes);
    return best.pin;
}

/** 步骤 2：在已知 RX 脚上找波特率 */
static uint32_t autodetect_baud(void)
{
    uint32_t bauds[MAX_CANDIDATES];
    static char buf[256];
    strncpy(buf, CONFIG_APP_EDK60_BRIDGE2_BAUD_LIST, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok != NULL && n < MAX_CANDIDATES;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        unsigned long v = strtoul(tok, NULL, 10);
        if (v >= 9600 && v <= 5000000) {
            bauds[n++] = (uint32_t)v;
        }
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "候选波特率列表为空或格式错误，保留当前 %u", (unsigned)BRIDGE_BAUD);
        return BRIDGE_BAUD;
    }

    ESP_LOGI(TAG, "===== 步骤 2/2：识别波特率（每个候选监听 %d ms）=====", BRIDGE_PROBE_MS);
    ESP_LOGI(TAG, "说明：ESP32-C6 没有硬件波特率自动检测，这里是软件穷举；"
                  "「字节数多 + 字节种类丰富」越像正常串流。");
    ESP_LOGI(TAG, "若你知道确切波特率，请在 menuconfig 里关掉自动识别并直接填值。");

    baud_probe_t best = { .baud = BRIDGE_BAUD, .bytes = 0, .distinct = 0 };

    for (int i = 0; i < n; i++) {
        if (bridge_uart_set_baud(bauds[i]) != ESP_OK) {
            ESP_LOGW(TAG, "  设为 %u 失败，跳过", (unsigned)bauds[i]);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        uart_flush_input(BRIDGE_UART_PORT);

        uint32_t bytes = 0;
        int distinct = 0;
        probe_bytes(&bytes, &distinct);

        ESP_LOGI(TAG, "  %7u bps : %6u 字节 / %3d 种字节%s",
                 (unsigned)bauds[i], (unsigned)bytes, distinct,
                 (bytes > 0) ? "" : "   (无数据)");

        /* 评分：字节数优先，字节种类作次要判据（同分时种类多的更可能是真数据） */
        if (bytes > best.bytes || (bytes == best.bytes && bytes > 0 && distinct > best.distinct)) {
            best.baud = bauds[i];
            best.bytes = bytes;
            best.distinct = distinct;
        }
    }

    if (best.bytes == 0) {
        ESP_LOGW(TAG, "所有候选波特率都没收到数据 —— 保留配置值 %u bps 继续运行",
                 (unsigned)BRIDGE_BAUD);
        bridge_uart_set_baud(BRIDGE_BAUD);
        return BRIDGE_BAUD;
    }

    ESP_LOGI(TAG, "==> 波特率识别结果：%u bps（%u 字节 / %d 种）",
             (unsigned)best.baud, (unsigned)best.bytes, best.distinct);
    ESP_LOGI(TAG, "如果这个结果不对，请在 menuconfig 里关掉自动识别并手填波特率。");
    bridge_uart_set_baud(best.baud);
    return best.baud;
}

#endif /* BRIDGE_AUTODETECT */

/* ---------------------------------------------------------------------------
 * 搬运任务
 * ------------------------------------------------------------------------- */

/** 雷达 → USB */
static void task_radar_to_usb(void *arg)
{
    (void)arg;
    static uint8_t buf[BRIDGE_CHUNK];
    bool warned = false;

    while (1) {
        int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(BRIDGE_RD_TIMEOUT_MS));
        if (n <= 0) {
            continue;
        }
        stats_add(&s_stats.radar_to_usb_bytes, (uint32_t)n);
        stats_add(&s_stats.radar_to_usb_chunks, 1);

        if (!usb_write(buf, (uint32_t)n) && !warned) {
            ESP_LOGW(TAG, "USB 写入未完全成功 —— 上位机可能没打开串口，或主机没在读。"
                          "数据仍会继续尝试发送（此提示只打印一次）");
            warned = true;
        }
    }
}

/** USB → 雷达（本板默认无 TX 脚，此时只做本地回环/统计） */
static void task_usb_to_radar(void *arg)
{
    (void)arg;
    static uint8_t buf[BRIDGE_CHUNK];

    while (1) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(BRIDGE_RD_TIMEOUT_MS));
        if (n <= 0) {
            continue;
        }
        stats_add(&s_stats.usb_to_radar_bytes, (uint32_t)n);
        stats_add(&s_stats.usb_to_radar_chunks, 1);

        if (BRIDGE_TX_PIN >= 0) {
            uart_write_bytes(BRIDGE_UART_PORT, (const char *)buf, n);
        } else {
            stats_add(&s_stats.usb_to_radar_dropped, (uint32_t)n);
        }

#if BRIDGE_LOCAL_ECHO
        if (BRIDGE_ECHO_PREFIX) {
            static const char prefix[] = "[LOOPBACK] ";
            usb_write((const uint8_t *)prefix, sizeof(prefix) - 1);
        }
        if (usb_write(buf, (uint32_t)n)) {
            stats_add(&s_stats.echo_bytes, (uint32_t)n);
        }
#endif
    }
}

/** 统计上报（走控制台，不污染 USB 数据流） */
static void task_stats(void *arg)
{
    (void)arg;
#if BRIDGE_STATS_SEC > 0
    while (1) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)BRIDGE_STATS_SEC * 1000));
        ESP_LOGI(TAG,
                 "[统计] 雷达→USB %u 字节/%u 次 | USB→雷达 %u 字节/%u 次 | "
                 "回显 %u 字节 | 丢弃(无TX脚) %u 字节",
                 (unsigned)stats_get(&s_stats.radar_to_usb_bytes),
                 (unsigned)stats_get(&s_stats.radar_to_usb_chunks),
                 (unsigned)stats_get(&s_stats.usb_to_radar_bytes),
                 (unsigned)stats_get(&s_stats.usb_to_radar_chunks),
                 (unsigned)stats_get(&s_stats.echo_bytes),
                 (unsigned)stats_get(&s_stats.usb_to_radar_dropped));
    }
#else
    vTaskDelete(NULL);
#endif
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */

void demo_radar_bridge_start(void)
{
    ESP_LOGI(TAG, "=== radar_bridge：雷达 ⇄ UART ⇄ USB串口 ⇄ PC 纯字节透传 ===");

ESP_LOGI(TAG, "数据通路：雷达 TX → %s(RX) → 原生 USB(IO%d/IO%d) → PC 上位机",
             BRIDGE_UART_PORT_NAME, EDK60_PIN_USB_DM, EDK60_PIN_USB_DP);
    ESP_LOGI(TAG, "日志走主控制台 UART0（H2 的 D0/D1，115200）；USB 上跑的是**干净的数据**。");
    ESP_LOGI(TAG, "参数：RX=IO%d%s 波特率=%u 自动识别=%s 本地回环=%s 搬运块=%d 字节",
             BRIDGE_RX_PIN,
             (BRIDGE_TX_PIN >= 0) ? "" : "（TX 未使用）",
             (unsigned)BRIDGE_BAUD,
             BRIDGE_AUTODETECT ? "开" : "关",
             BRIDGE_LOCAL_ECHO ? "开" : "关",
             BRIDGE_CHUNK);

    /* 0) LED 自检：两颗可控灯各闪一下，证明板子活着 */
    if (edk60_led_init() == ESP_OK) {
        edk60_led_set_all(false);
        for (int i = 0; i < EDK60_LED_GPIO_COUNT; i++) {
            edk60_led_set(i, true);
            vTaskDelay(pdMS_TO_TICKS(120));
            edk60_led_set(i, false);
        }
    }

    /* 1) USB 驱动（幂等：控制台若已占用则复用） */
    if (bridge_usb_install() != ESP_OK) {
        ESP_LOGE(TAG, "USB 不可用，透传无法工作；请检查 USB-C 是否插好、"
                      "以及 menuconfig 里 Channel for console output 的配置。");
        return;
    }

    /* 2) 雷达电源（可选，含开机使能） */
    radar_power_setup();

    /* 3) UART */
    if (bridge_uart_install() != ESP_OK) {
        ESP_LOGE(TAG, "%s 初始化失败 —— 请检查 RX=IO%d 是否被其它外设占用",
                 BRIDGE_UART_PORT_NAME, BRIDGE_RX_PIN);
        return;
    }

    /* ===== 开机与雷达握手（必需，否则永远没有体征数据）=====
     * 依据 docs/EDV151-N-H01串口通用协议.md：
     *   · turn_on / turn_off 是射频开关，**默认关闭** ⇒ 不开射频就没有 vital_signs；
     *   · set_time <uint32> 设置时间戳（雷达每小时会主动 get_time 请求校时）；
     *   · set_para 4:<秒> 设置上报周期。
     * 实测（2026-10-15）：模组 RX = IO5，发封包后回应 "ok"。
     *
     * 注意：本段必须在**启动透传任务之前**执行 —— 否则 uart_read_bytes/flush
     * 会与透传任务争抢 UART 接收锁而死锁（本项目踩过）。
     * 握手完成后本 demo 只做字节透传，不再解析协议。
     * ============================================================ */
#if BRIDGE_HAS_RADAR_TX
    {
        static const uint8_t cmd_turn_on[] = { 0x5A, 0x0B, 0x81,
                                               't','u','r','n','_','o','n', 0x93 };
        static const uint8_t cmd_set_time[] = { 0x5A, 0x17, 0x82,
                                               's','e','t','_','t','i','m','e',' ',
                                               '1','7','9','3','0','0','0','0','0','0', 0xCB };
        static const uint8_t cmd_period[]  = { 0x5A, 0x10, 0x83,
                                               's','e','t','_','p','a','r','a',' ','4',':','1', 0xE9 };
        static const uint8_t cmd_range[]   = { 0x5A, 0x17, 0x84,
                                               's','e','t','_','p','a','r','a',' ',
                                               '5',':','3','0',',','6',':','1','5','0', 0xCE };
        /* 增益与增强模式：**这两条是能否算出体征的关键**
         * 实测（2026-10-15）：出厂 gain=4/boost=0 时，人静坐的体动仍高达 20~60，
         * 算法永远算不出心率/呼吸（协议要求静息体动 < 5）。
         * 按官方文档推荐改成 gain=0.5、boost=2 后，体动降到 1~6，
         * 心率 84bpm、呼吸率 18bpm 均成功输出。
         * 若换场地/换姿态后仍出不了数，优先调这两个值。 */
        static const uint8_t cmd_gain[]    = { 0x5A, 0x10, 0x85,
                                               's','e','t','_','g','a','i','n',' ',
                                               '0','.','5', 0xF8 };
        static const uint8_t cmd_boost[]   = { 0x5A, 0x0F, 0x86,
                                               's','e','t','_','b','o','o','s','t',' ','2', 0x99 };

        vTaskDelay(pdMS_TO_TICKS(1200));     /* 等模组初始化（上电到 radar_ready 约 1 秒） */

        struct { const uint8_t *buf; int len; const char *name; } handshake[] = {
            { cmd_turn_on,  sizeof(cmd_turn_on),  "turn_on（开射频）" },
            { cmd_set_time, sizeof(cmd_set_time), "set_time（校时）" },
            { cmd_period,   sizeof(cmd_period),   "set_para 4:1（上报周期 1 秒）" },
            { cmd_range,    sizeof(cmd_range),    "set_para 5:30,6:150（检测范围 30~150cm）" },
            { cmd_gain,     sizeof(cmd_gain),     "set_gain 0.5（降低灵敏度，静息体动<5）" },
            { cmd_boost,    sizeof(cmd_boost),    "set_boost 2（中心增强）" },
        };
        for (size_t i = 0; i < sizeof(handshake) / sizeof(handshake[0]); i++) {
            uart_flush_input(BRIDGE_UART_PORT);
            int w = uart_write_bytes(BRIDGE_UART_PORT, handshake[i].buf, handshake[i].len);
            uint8_t ack[64];
            int an = uart_read_bytes(BRIDGE_UART_PORT, ack, sizeof(ack), pdMS_TO_TICKS(500));
            bool ok = (an >= 6 && ack[3] == 'o' && ack[4] == 'k');
            ESP_LOGI(TAG, "握手 %-34s 写%2d字节  应答%d字节 %s",
                     handshake[i].name, w, an, ok ? "ok" : "(未取到 ok)");
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        ESP_LOGI(TAG, "握手完成：模组应开始定时上报 vital_signs 体征帧");
    }
#endif


    /* 4) 自动识别（可选） */
    int rx_pin = BRIDGE_RX_PIN;
    uint32_t baud = BRIDGE_BAUD;
#if BRIDGE_AUTODETECT
    int detected = autodetect_rx_pin();
    if (detected >= 0) {
        rx_pin = detected;
        baud = autodetect_baud();
    } else {
        /* 引脚扫描没找到数据源，但**不要就此罢休**：
         * 如果模组波特率与扫描时用的波特率差太多，ESP32 的 UART 会因帧错误
         * 把字节全部丢弃，表现同样是"0 字节"，很容易误判成"模组不输出"。
         * 所以这里再在配置的 RX 脚上把候选波特率**逐个试一遍**并打印字节数，
         * 让「波特率不对」和「真的没信号」能区分开。 */
        ESP_LOGW(TAG, "自动识别未找到数据源；先在 RX=IO%d 上做一次**波特率扫描**，"
                      "排除「波特率不匹配导致 UART 丢弃全部字节」这种可能",
                 BRIDGE_RX_PIN);
        bridge_uart_set_rx_pin(BRIDGE_RX_PIN);

        int bauds[MAX_CANDIDATES];
        int bn = parse_csv_ints(CONFIG_APP_EDK60_BRIDGE2_BAUD_LIST, bauds, MAX_CANDIDATES);
        uint32_t best_baud = 0;
        uint32_t best_bytes = 0;
        for (int i = 0; i < bn; i++) {
            if (bauds[i] <= 0) {
                continue;
            }
            bridge_uart_set_baud((uint32_t)bauds[i]);
            vTaskDelay(pdMS_TO_TICKS(20));
            uart_flush_input(BRIDGE_UART_PORT);
            uint32_t bytes = 0;
            int distinct = 0;
            probe_bytes(&bytes, &distinct);
            ESP_LOGW(TAG, "  波特率 %7d : 收到 %6u 字节 / %3d 种字节",
                     bauds[i], (unsigned)bytes, distinct);
            if (bytes > best_bytes) {
                best_bytes = bytes;
                best_baud = (uint32_t)bauds[i];
            }
        }
        if (best_baud != 0) {
            ESP_LOGW(TAG, "==> 波特率扫描有结果：%u bps 收到最多字节（%u）—— 很可能是这个",
                     (unsigned)best_baud, (unsigned)best_bytes);
            baud = best_baud;
        } else {
            ESP_LOGW(TAG, "==> 波特率全部为 0 字节 ⇒ 该脚上确实没有UART信号（不是波特率问题）");
            baud = BRIDGE_BAUD;
        }
        bridge_uart_set_baud(baud);
        ESP_LOGW(TAG, "将按 RX=IO%d, %u bps 继续运行", BRIDGE_RX_PIN, (unsigned)baud);
    }
#else
    ESP_LOGI(TAG, "自动识别已关闭，直接使用配置值 RX=IO%d, %u bps",
             BRIDGE_RX_PIN, (unsigned)BRIDGE_BAUD);
#endif

    /* 5) 统计任务（走控制台） */
    xTaskCreate(task_stats, "br_stats", 3072, NULL, 3, NULL);

    /* 6) 双向搬运任务 */
    if (BRIDGE_TX_PIN >= 0) {
        ESP_LOGI(TAG, "双向透传已启用（TX=IO%d）", BRIDGE_TX_PIN);
    } else {
        ESP_LOGI(TAG, "单向透传：雷达 → PC（本板 H4 未引出 MCU→模组 的数据线）");
    }
    xTaskCreate(task_radar_to_usb, "br_r2u", 4096, NULL, 6, NULL);
    xTaskCreate(task_usb_to_radar, "br_u2r", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "----------------------------------------------");
    ESP_LOGI(TAG, "现在请在 PC 上打开 USB 对应的串口（设备管理器 → 端口 → USB 串行设备），");
    ESP_LOGI(TAG, "波特率随意填（USB CDC 不依赖波特率），即可看到雷达原始数据。");
    ESP_LOGI(TAG, "最终生效：RX=IO%d，波特率=%u bps", rx_pin, (unsigned)baud);

    if (BRIDGE_LOCAL_ECHO) {
        ESP_LOGI(TAG, "本地回环自测：开 —— 在串口助手里发字符会原样回来（用于验证 USB 通路）；"
                      "确认没问题后建议在 menuconfig 里关掉它。");
    }
    ESP_LOGI(TAG, "----------------------------------------------");
}





