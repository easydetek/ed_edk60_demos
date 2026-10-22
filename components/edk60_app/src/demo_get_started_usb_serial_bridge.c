/*
 * EDK60 Demo 4.3 —— usb_serial_bridge
 * ===========================================================================
 * 编号/名称 : 4.3 usb_serial_bridge（低功耗与存储组）
 * 目的      : 回答「这块板子能不能当 USB 转串口用」—— 能，而且不需要任何外部
 *             转换芯片：把 PC 侧的原生 USB CDC 串口与雷达侧的 UART1 做双向桥接，
 *             同时给出本板自己写往 USB 的数据（含本地回环自测）。
 *
 * 硬件依据（原理图 docs/SCH_EDK60_VA.pdf，EDK60_VA V1.0；网络名照抄图纸）
 * ---------------------------------------------------------------------------
 *  * USB-C 连接器（GT-USB-7010ASV）的 D-/D+ 直接接在 ESP32-C6 的 IO12(D-)/IO13(D+)
 *    —— 这两脚是 C6 **原生 USB Serial/JTAG 控制器**的固定引脚（GPIO 优先级 P1），
 *    板上**没有 CH340 / CP2102 之类的 USB 转串口芯片**。也就是说「USB 转串口」
 *    这个能力是芯片自带的，本 demo 只是把它用起来（原生 USB 免驱、免晶振、
 *    免电平转换，最高 12Mbps）。
 *  * 雷达引脚（**2026-10-15 上板实测定论，已推翻早期读图推断**）：
 *        模组 TX -> MCU      = IO4    （H3 的 T 脚）
 *        模组 RX <- MCU      = IO5    （H3 的 A 脚）
 *        雷达电源使能 EN     = IO14   （U4.EN，输出高 = 3.3V_RADAR 上电）
 *        橙色指示灯（丝印 L）= IO15   （仅指示灯，与电源 EN 无关）
 *    模组必须插在 H3：H3/H4 信号相同但顺序相反，H4 的 T 与 EN 同网络，会冲突。
 *
 * !! 本 demo 的定位（务必先读，否则会误判「桥不通」）
 * ---------------------------------------------------------------------------
 *  * 本 demo 只做「USB <-> 雷达 UART」的**字节往返演示**，RX=IO4、TX=IO5 都已经接好，
 *    所以双向在物理上都是通的。
 *  * 但模组射频**默认关闭**，不发 turn_on 时它只会主动发 radar_ready / get_time，
 *    因此你在 PC 侧看到的往往只有这两类握手帧 —— 这不是桥不通，是模组还没开射频。
 *  * 要拿到心率/呼吸率等体征数据，请用 demo 5.1 radar_bridge：
 *    它会自动完成 turn_on / set_gain / set_boost 等 6 条握手命令。
 *  * PC 侧收到的内容全部由本板 USB 端点写出：既包括转发过来的雷达数据，也包括
 *    本板日志（本工程 sdkconfig.defaults 把 USB Serial/JTAG 配成了控制台的
 *    "第二输出口"）以及本地回环自测的回显。看到内容杂乱时，先怀疑日志混入。
 *  * 若要让「USB → 雷达」真正可用，需要硬件上给雷达侧的 MCU-TX 分配一个空闲
 *    GPIO 并接到 H4 对应脚（H4 的引脚分配以原理图为准，改板后再同步 BSP）。
 *
 * PC 侧使用说明
 * --------------
 *  1) 用 USB-C 线把 EDK60 插到电脑。板子没有 USB 转串口芯片，电脑看到的是
 *     ESP32-C6 自带的 USB 串行设备（免驱）：
 *        Windows: 「设备管理器 → 端口(COM 和 LPT)」里出现 "USB 串行设备 (COMx)"
 *                 / "USB Serial Device (COMx)"；VID/PID 为 303A:1001
 *                 （**以实测为准**，可用 USBTreeView / 设备管理器硬件 ID 核实）；
 *        Linux:   /dev/ttyACM0（lsusb 可见 303a:1001 "Espressif USB JTAG/serial
 *                 debug unit"）
 *        macOS:   /dev/cu.usbmodemXXXX
 *  2) 波特率随便填：USB CDC 是"真 USB"而不是 UART，波特率对 USB 侧不生效。
 *     但为了和雷达侧的对数习惯一致，建议填 Kconfig 里的雷达波特率
 *     （默认 256000，见 APP_EDK60_RADAR_UART_BAUD）。
 *  3) 打开任意串口助手：
 *        Windows: putty -serial COM3        （或 MobaXterm / sscom / 串口助手）
 *        Linux  : screen /dev/ttyACM0 256000
 *        macOS  : screen /dev/cu.usbmodemXXXX 256000
 *  4) 第一步先用「本地回环自测」确认 USB 通路本身是好的：在这种模式下，你在
 *     串口助手里敲什么，就会原样看到什么（**不需要插雷达模组**）。
 *     确认通路没问题后，再按下面的情况调整：
 *        - 接真实雷达模组做纯桥接 → 建议关掉 CONFIG_APP_EDK60_BRIDGE_LOCAL_ECHO，
 *          否则回显会和雷达数据混在同一个串口里；
 *        - 只想肉眼看清字节 → 关掉 CONFIG_APP_EDK60_BRIDGE_RAW_PASSTHROUGH，
 *          切到「带帧头调试视图」（十六进制 + ASCII）。
 *  5) 板子在 USB 没插/只接了充电器时也能跑：`usb_serial_jtag_is_connected()`
 *     只把 SOF 包当作"已连接"，所以接充电宝不会被误判成连上了主机。
 *
 * 观察点
 * ------
 *  1) 控制台（UART0，115200-8-N-1，**不受本 demo 影响**）：启动时打印 USB 驱动
 *     安装结果、桥接模式、硬件限制提示；之后每 N 秒打印一次双向统计与接线状态。
 *  2) 串口助手里应看到：回环自测的回显（默认开启）/ 雷达数据 / 带帧头调试视图。
 *  3) 插拔 USB：控制台会打印 USB 连接状态变化（利用 `usb_serial_jtag_is_connected()`；
 *     本版本驱动头文件里**没有** DTR/RTS 变化回调 API，故不做臆造调用）。
 *  4) 未插雷达模组时，UART1 方向一直没数据，只打印一次接线提示，不 panic、
 *     不 assert、不刷屏。
 *
 * 可配置项（menuconfig -> "EDK60 Demo 参数（雷达/USB 桥）"）
 * ---------------------------------------------------------
 *  APP_EDK60_RADAR_UART_BAUD            雷达波特率（默认 256000）
 *  APP_EDK60_BRIDGE_RAW_PASSTHROUGH     原样透传（默认 y；关=带帧头调试视图）
 *  APP_EDK60_BRIDGE_LOCAL_ECHO          USB 本地回环自测（默认 y）
 *  APP_EDK60_BRIDGE_STATS_SEC           统计打印周期（默认 10s）
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "demo_usb_bridge";

/* ---------------------------------------------------------------------------
 * 常量
 * ------------------------------------------------------------------------- */

/** USB Serial/JTAG 驱动缓冲区 */
#define BRIDGE_USB_RX_BUF        1024
#define BRIDGE_USB_TX_BUF        2048

/** 雷达侧 UART1（UART0 被控制台占用） */
#define BRIDGE_UART_PORT         UART_NUM_1
#define BRIDGE_UART_BAUD         CONFIG_APP_EDK60_RADAR_UART_BAUD
#define BRIDGE_UART_RX_BUF       4096
#define BRIDGE_UART_TX_BUF       2048

#define BRIDGE_CHUNK             256     /* 单次搬运的最大字节数 */
#define BRIDGE_IO_TIMEOUT_MS     100     /* 两个方向各自的读取超时 */
#define BRIDGE_STATS_MS          (CONFIG_APP_EDK60_BRIDGE_STATS_SEC * 1000)
#define BRIDGE_TASK_STACK        4096

/** 帧头调试视图里每行显示多少字节 */
#define BRIDGE_FRAME_COLS        16

/* Kconfig 是 bool：未选中时符号根本不定义，所以用 #ifdef 转成 0/1，
 * 避免在表达式里直接引用未定义的宏。 */
#ifdef CONFIG_APP_EDK60_BRIDGE_RAW_PASSTHROUGH
#define BRIDGE_MODE_RAW          (1)
#else
#define BRIDGE_MODE_RAW          (0)
#endif

#ifdef CONFIG_APP_EDK60_BRIDGE_LOCAL_ECHO
#define BRIDGE_LOCAL_ECHO        (1)
#else
#define BRIDGE_LOCAL_ECHO        (0)
#endif

/* ---------------------------------------------------------------------------
 * 双向统计
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t usb_rx_bytes;   /* PC → 板：从 USB 收到的字节 */
    uint32_t usb_rx_pkts;
    uint32_t uart_rx_bytes;  /* 雷达 → 板：从 UART1 收到的字节 */
    uint32_t uart_rx_pkts;
    uint32_t usb_tx_bytes;   /* 板 → PC：写到 USB 的字节（雷达数据 + 回显 + 帧头） */
    uint32_t usb_tx_pkts;
    uint32_t echo_bytes;     /* 其中来自本地回环自测的字节 */
} bridge_stats_t;

static bridge_stats_t s_stats;
static SemaphoreHandle_t s_stats_lock;

/** 统计自增（两个任务都会写，用互斥锁保护；锁创建失败时退化为无锁自增） */
static void bridge_count(uint32_t *field, uint32_t n)
{
    if (n == 0) {
        return;
    }
    if (s_stats_lock != NULL &&
        xSemaphoreTake(s_stats_lock, portMAX_DELAY) == pdTRUE) {
        *field += n;
        xSemaphoreGive(s_stats_lock);
        return;
    }
    *field += n;
}

/* ---------------------------------------------------------------------------
 * 字节搬运助手
 * ------------------------------------------------------------------------- */

/** 把数据写到 USB（整包写完或超时丢弃，返回实际写出的字节数） */
static size_t bridge_usb_write(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;

    while (left > 0) {
        int w = usb_serial_jtag_write_bytes(p, left, pdMS_TO_TICKS(BRIDGE_IO_TIMEOUT_MS));
        if (w <= 0) {
            ESP_LOGW(TAG, "USB 写入失败/超时，丢弃剩余 %u 字节", (unsigned)left);
            break;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }
    return len - left;
}

/**
 * 「带帧头调试视图」：把一批字节以 帧头 + 十六进制 + ASCII 的形式写到 USB。
 * 只影响 USB 侧的可读输出，不影响写进 UART1 的原始字节。
 * 返回写出的字节数。
 */
static size_t bridge_emit_framed(const char *dir, const uint8_t *data, size_t len)
{
    static const char hexd[] = "0123456789ABCDEF";
    char head[80];
    char line[BRIDGE_FRAME_COLS * 4 + 8];
    size_t written = 0;

    int hl = snprintf(head, sizeof(head), "\n[%s] %u 字节\n", dir, (unsigned)len);
    if (hl > 0) {
        written += bridge_usb_write(head, (size_t)hl);
    }

    for (size_t off = 0; off < len; off += BRIDGE_FRAME_COLS) {
        size_t n = (len - off) > BRIDGE_FRAME_COLS ? BRIDGE_FRAME_COLS : (len - off);
        size_t p = 0;

        for (size_t i = 0; i < n; i++) {
            uint8_t b = data[off + i];
            line[p++] = hexd[(b >> 4) & 0x0F];
            line[p++] = hexd[b & 0x0F];
            line[p++] = ' ';
        }
        line[p++] = '|';
        line[p++] = ' ';
        for (size_t i = 0; i < n; i++) {
            uint8_t b = data[off + i];
            line[p++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        line[p++] = '\n';

        written += bridge_usb_write(line, p);
    }
    return written;
}

/* ---------------------------------------------------------------------------
 * 方向 1：USB（PC） → UART1（雷达）
 * ------------------------------------------------------------------------- */

static void bridge_usb_to_uart_task(void *arg)
{
    (void)arg;
    uint8_t buf[BRIDGE_CHUNK];

    while (1) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf),
                                           pdMS_TO_TICKS(BRIDGE_IO_TIMEOUT_MS));
        if (n <= 0) {
            continue;   /* 0 = 超时（PC 没发东西），负值忽略；都不影响其它方向 */
        }

        bridge_count(&s_stats.usb_rx_bytes, (uint32_t)n);
        bridge_count(&s_stats.usb_rx_pkts, 1);

        /* ① 原样写进 UART1。
         *    注意：当前硬件下 UART1 的 TX 未布线（见文件头「硬件限制」），
         *    这些字节只会进入发送 FIFO，不会到达雷达模组。 */
        int written = uart_write_bytes(BRIDGE_UART_PORT, buf, (size_t)n);
        if (written < 0) {
            ESP_LOGW(TAG, "UART1 写入失败(%d)，本批 %d 字节未入队", written, n);
        }

        /* ② USB 侧的可视化 */
        if (BRIDGE_MODE_RAW) {
            if (BRIDGE_LOCAL_ECHO) {
                /* 本地回环自测：收到什么就原样回什么，
                 * 用来在「没插雷达模组」时先证明 USB 原生串口本身是通的。 */
                size_t w = bridge_usb_write(buf, (size_t)n);
                bridge_count(&s_stats.usb_tx_bytes, (uint32_t)w);
                bridge_count(&s_stats.usb_tx_pkts, w ? 1u : 0u);
                bridge_count(&s_stats.echo_bytes, (uint32_t)w);
            }
        } else {
            size_t w = bridge_emit_framed("USB->RADAR", buf, (size_t)n);
            bridge_count(&s_stats.usb_tx_bytes, (uint32_t)w);
            bridge_count(&s_stats.usb_tx_pkts, w ? 1u : 0u);
            bridge_count(&s_stats.echo_bytes, (uint32_t)w);
        }
    }
}

/* ---------------------------------------------------------------------------
 * 方向 2：UART1（雷达） → USB（PC）
 * ------------------------------------------------------------------------- */

static void bridge_uart_to_usb_task(void *arg)
{
    (void)arg;
    uint8_t buf[BRIDGE_CHUNK];

    while (1) {
        int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf),
                                pdMS_TO_TICKS(BRIDGE_IO_TIMEOUT_MS));
        if (n < 0) {
            /* 未插模组时会一直是 0（超时）；出现负值说明驱动异常，继续跑不 assert */
            ESP_LOGW(TAG, "UART1 读取异常(%d)，忽略并继续", n);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (n == 0) {
            continue;
        }

        bridge_count(&s_stats.uart_rx_bytes, (uint32_t)n);
        bridge_count(&s_stats.uart_rx_pkts, 1);

        size_t w;
        if (BRIDGE_MODE_RAW) {
            w = bridge_usb_write(buf, (size_t)n);
        } else {
            w = bridge_emit_framed("RADAR->USB", buf, (size_t)n);
        }
        bridge_count(&s_stats.usb_tx_bytes, (uint32_t)w);
        bridge_count(&s_stats.usb_tx_pkts, w ? 1u : 0u);
    }
}

/* ---------------------------------------------------------------------------
 * 周期统计 + USB 连接状态
 * ------------------------------------------------------------------------- */

static void bridge_stats_task(void *arg)
{
    (void)arg;
    bool was_connected = usb_serial_jtag_is_connected();
    bool uart_hint_done = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_STATS_MS));

        bridge_stats_t snap = {0};
        if (s_stats_lock != NULL &&
            xSemaphoreTake(s_stats_lock, portMAX_DELAY) == pdTRUE) {
            snap = s_stats;
            xSemaphoreGive(s_stats_lock);
        }

        ESP_LOGI(TAG, "桥接统计: PC→板 %" PRIu32 " 字节/%" PRIu32 " 包（写入 UART1）| "
                      "雷达→板 %" PRIu32 " 字节/%" PRIu32 " 包（转发到 USB）| "
                      "板→PC 共 %" PRIu32 " 字节（其中本地回显 %" PRIu32 "）| EN=%d",
                 snap.usb_rx_bytes, snap.usb_rx_pkts,
                 snap.uart_rx_bytes, snap.uart_rx_pkts,
                 snap.usb_tx_bytes, snap.echo_bytes,
                 (int)edk60_radar_power_is_on());

        /* USB 主机连接状态变化（本版本只有 is_connected()，没有 DTR/RTS 回调） */
        bool connected = usb_serial_jtag_is_connected();
        if (connected != was_connected) {
            was_connected = connected;
            ESP_LOGI(TAG, "USB Serial/JTAG %s：%s",
                     connected ? "已连接主机" : "已断开主机",
                     connected ? "PC 已枚举并保持 SOF，串口助手可以打开了"
                               : "只接充电器不会算作已连接（没有 SOF 包）");
        }

        /* 雷达方向一直没数据时提醒一次就够，不刷屏 */
        if (!uart_hint_done && snap.uart_rx_bytes == 0) {
            uart_hint_done = true;
            ESP_LOGW(TAG, "UART1(IO%d) 至今没有数据：①H4 未插雷达模组；"
                          "②模组波特率≠%d；③模组 TX 未接到 IO%d；"
                          "④3.3V_RADAR 未使能（橙色 LED4 不亮）或模组未就绪",
                     (int)EDK60_PIN_RADAR_UART_RX, BRIDGE_UART_BAUD,
                     (int)EDK60_PIN_RADAR_UART_RX);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Demo 入口
 * ------------------------------------------------------------------------- */

void demo_usb_serial_bridge_start(void)
{
    ESP_LOGI(TAG, "=== usb_serial_bridge：C6 原生 USB Serial/JTAG ⇄ 雷达 UART1 桥接 ===");
    ESP_LOGI(TAG, "硬件依据：USB-C 的 D-/D+ 直接接 IO%d(USB_DN)/IO%d(USB_DP)"
                  "（ESP32-C6 原生 USB Serial/JTAG，固定引脚），"
                  "板上无 CH340/CP2102 —— USB 转串口由芯片自己做",
             (int)EDK60_PIN_USB_DM, (int)EDK60_PIN_USB_DP);
    ESP_LOGI(TAG, "雷达侧：UART1 RX=IO%d（H4-T）；TX 保持 UART_PIN_NO_CHANGE（未布线），"
                  "即「雷达→USB」连通、「USB→雷达」不下到引脚，详见文件头硬件限制",
             (int)EDK60_PIN_RADAR_UART_RX);

    /* --- 1. 雷达电源：初始化（默认为关）+ 使能，让模组具备输出数据的前提 --- */
    ESP_ERROR_CHECK(edk60_radar_power_init());
    ESP_ERROR_CHECK(edk60_radar_power_on());
    ESP_LOGI(TAG, "3.3V_RADAR：EN(IO%d) 实测=%d（1=已使能，橙色 LED4 应同步点亮；"
                  "红色 LED5 反映 3.3V_RADAR 轨是否真的有电）",
             (int)EDK60_PIN_RADAR_PWR_EN, (int)edk60_radar_power_is_on());

    /* --- 2. 安装 USB Serial/JTAG 驱动（幂等：已安装就复用） --- */
    if (usb_serial_jtag_is_driver_installed()) {
        ESP_LOGI(TAG, "USB Serial/JTAG 驱动已由 IDF 或其它模块安装 → 直接复用，不重复安装");
    } else {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_cfg.rx_buffer_size = BRIDGE_USB_RX_BUF;
        usb_cfg.tx_buffer_size = BRIDGE_USB_TX_BUF;

        esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
        if (err == ESP_ERR_INVALID_STATE) {
            /* 主控制台配成 USB Serial/JTAG 时，驱动可能已被 IDF 抢先安装 —— 
             * 这不是错误，直接复用现有实例即可。 */
            ESP_LOGI(TAG, "USB Serial/JTAG 驱动已安装（ESP_ERR_INVALID_STATE）→ 复用现有实例");
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB Serial/JTAG 驱动安装失败: %s —— 桥接不可用，"
                          "请检查是否与其它 USB 组件冲突", esp_err_to_name(err));
            return;
        } else {
            ESP_LOGI(TAG, "USB Serial/JTAG 驱动安装成功（RX 缓冲 %d / TX 缓冲 %d 字节）",
                     BRIDGE_USB_RX_BUF, BRIDGE_USB_TX_BUF);
        }
    }
    /* 注意：安装本驱动**不改变**控制台走向 —— 本工程主控制台仍是 UART0
     * （IO16/IO17），ESP_LOG 依旧从那里输出；USB 只是控制台的第二输出口。 */

    /* --- 3. UART1：只给 RX 布线 --- */
    const uart_config_t cfg = {
        .baud_rate = BRIDGE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART_PORT, BRIDGE_UART_RX_BUF,
                                        BRIDGE_UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_PORT, &cfg));
    /* TX = IO5 已接好（实测模组 RX = IO5），可向模组下发命令 */
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT, UART_PIN_NO_CHANGE,
                                 EDK60_PIN_RADAR_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_rx_timeout(BRIDGE_UART_PORT, 5));
    ESP_LOGI(TAG, "UART1 就绪：RX=IO%d，%d-8-N-1，TX 未布线",
             (int)EDK60_PIN_RADAR_UART_RX, BRIDGE_UART_BAUD);

    /* --- 4. 统计锁 --- */
    s_stats_lock = xSemaphoreCreateMutex();
    if (s_stats_lock == NULL) {
        ESP_LOGW(TAG, "统计互斥锁创建失败，统计值可能略有偏差（不影响桥接功能）");
    }

    /* --- 5. 桥接任务（三个都建成功才算就绪；各自独立判断，避免漏建） --- */
    bool ok = true;
    ok = (xTaskCreate(bridge_usb_to_uart_task, "usb2radar", BRIDGE_TASK_STACK,
                      NULL, 5, NULL) == pdPASS) && ok;
    ok = (xTaskCreate(bridge_uart_to_usb_task, "radar2usb", BRIDGE_TASK_STACK,
                      NULL, 5, NULL) == pdPASS) && ok;
    ok = (xTaskCreate(bridge_stats_task, "bridge_stats", BRIDGE_TASK_STACK,
                      NULL, 3, NULL) == pdPASS) && ok;
    if (!ok) {
        ESP_LOGE(TAG, "桥接任务创建失败（内存不足？）");
        return;
    }

    /* --- 6. 运行参数汇总 --- */
    ESP_LOGI(TAG, "桥接模式: %s | 本地回环自测: %s | 统计周期: %d 秒",
             BRIDGE_MODE_RAW ? "原样直通（RAW）" : "带帧头调试视图（HEX+ASCII）",
             BRIDGE_LOCAL_ECHO ? "开（不插雷达模组也能看到回显，用于验证 USB 通路）" : "关",
             CONFIG_APP_EDK60_BRIDGE_STATS_SEC);
    ESP_LOGI(TAG, "PC 侧：用 USB-C 线连接电脑后会出现一个原生 USB 串行设备"
                  "（Windows 显示为「USB 串行设备 (COMx)」，VID/PID 303A:1001，以实测为准；"
                  "Linux 为 /dev/ttyACM0），波特率随便填即可");
    if (BRIDGE_MODE_RAW && BRIDGE_LOCAL_ECHO) {
        ESP_LOGI(TAG, "现在就可以在串口助手里敲字：默认开启本地回环，你会原样看到它");
    }
    ESP_LOGI(TAG, "提示：USB 同时是本工程控制台的第二输出口，串口助手里可能混有本板日志；"
                  "做纯桥接时建议关掉 APP_EDK60_BRIDGE_LOCAL_ECHO 并提高日志级别");
}
