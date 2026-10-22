/*
 * EDK60 Demo 3.1 —— radar_power（板载互动组）
 *
 * ==========================================================================
 * 本 demo 演示什么
 * ==========================================================================
 *   1) 用 IO14（U4 的 EN）把雷达电源 3.3V_RADAR 关掉 / 打开；
 *   2) 用丝印 "L" 的橙色指示灯（IO15）做肉眼校验；
 *   3) 打开后在 UART1（RX=IO4 / TX=IO5 @921600）上收一小段雷达原始数据，
 *      **证明「电源打开」这一步确实生效**（电源没开时收不到任何字节）；
 *   4) 按 BOOT 键（IO9）切换电源通断，每次打印完整状态。
 *
 * ==========================================================================
 * 硬件事实（2026-10-15 上板实测确认，勿凭图纸改写）
 * ==========================================================================
 *   模组 TX -> MCU      = IO4    （H3 的 T 脚）
 *   模组 RX <- MCU      = IO5    （H3 的 A 脚）
 *   雷达电源使能 EN     = IO14   （U4 = AP7362-33SP-13；输出高 = 3.3V_RADAR 上电）
 *   橙色指示灯（丝印 L）= IO15   （**仅指示灯，与电源 EN 无关**）
 *
 *   ⚠ 早期版本按原理图推断「EN=IO15、雷达 RX=IO14」，**两个都错**。实测证据：
 *     · IO15 拉高 → 橙灯亮，但丝印 "RADAR" 的红灯不亮（3.3V_RADAR 没建立）；
 *     · IO14 拉高/拉低 → 红灯同步亮/灭（连续 5 次）⇒ EN = IO14；
 *     · 在 IO4 上收到合法帧 `5A 0F 00 "radar_ready" 05` ⇒ 雷达 TX = IO4；
 *     · 向 IO5 发 `get_version` 收到回应 `5A 07 80 "166" EC` ⇒ 模组 RX = IO5。
 *
 *   模组必须插在 **H3**：H3/H4 信号相同但顺序相反，H4 的 T 与 EN 同网络，
 *   插 H4 会导致电源与数据冲突。
 *
 * ==========================================================================
 * 关于「为什么只收数据、不发命令」
 * ==========================================================================
 *   本 demo 只做电源与链路的**自检**，因此只关心「开电后有没有字节进来」。
 *   要真正拿到体征数据（心率/呼吸率），请用 demo 5.1 radar_bridge：
 *   它会在开机时自动发 turn_on / set_gain / set_boost 等 6 条握手命令，
 *   把模组调到能算出体征的状态（细节见 docs/EDK60-pinmap.md 第二节）。
 *
 *   注意：模组射频**默认关闭**。只上电、不发 turn_on 时，模组仍会主动发
 *   radar_ready / get_time 两类握手帧 —— 本 demo 收到它们同样能证明
 *   「电源 + UART 通路都通了」。
 * ==========================================================================
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "edk60_board.h"
#include "edk60_demo.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "radar_power";

/* 自检用的 UART（与 radar_bridge 用同一对引脚，但这是独立 demo） */
#define SELF_UART_PORT   UART_NUM_1
#define SELF_UART_BAUD   921600
#define SELF_RX_PIN      EDK60_PIN_RADAR_UART_RX   /* IO4 */
#define SELF_TX_PIN      EDK60_PIN_RADAR_UART_TX   /* IO5 */

/* ---------------------------------------------------------------------------
 * 状态打印
 * ------------------------------------------------------------------------- */

static void print_state(const char *stage)
{
    bool en_on   = edk60_radar_power_is_on();
    int lvl_en   = gpio_get_level(EDK60_PIN_RADAR_PWR_EN);
    int lvl_led  = gpio_get_level(EDK60_PIN_RADAR_LED);

    printf("\n[%s]\n", stage);
    printf("  电源使能 EN : IO%-2d 输出=%d 回读=%d → 3.3V_RADAR %s\n",
           (int)EDK60_PIN_RADAR_PWR_EN, en_on ? 1 : 0, lvl_en,
           en_on ? "已上电（丝印 RADAR 红灯应亮）" : "已关断（红灯应灭）");
    printf("  橙色指示灯  : IO%-2d 电平=%d （仅指示，与电源无关）\n",
           (int)EDK60_PIN_RADAR_LED, lvl_led);
}

/* ---------------------------------------------------------------------------
 * UART 自检：开电后能不能收到雷达字节
 * ------------------------------------------------------------------------- */

static bool self_uart_install(void)
{
    uart_config_t cfg = {
        .baud_rate  = SELF_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(SELF_UART_PORT, 4096, 1024, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART1 驱动安装失败: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(SELF_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART1 参数配置失败: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(SELF_UART_PORT, SELF_TX_PIN, SELF_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART1 引脚设置失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "UART1 就绪：RX=IO%d（模组TX） TX=IO%d（模组RX）@%d bps",
             (int)SELF_RX_PIN, (int)SELF_TX_PIN, SELF_UART_BAUD);
    return true;
}

/** 在 UART 上监听 ms 毫秒，返回收到的字节数，并打印前若干字节 */
static int self_uart_listen(int ms, const char *why)
{
    uint8_t buf[128];
    uart_flush_input(SELF_UART_PORT);
    int n = uart_read_bytes(SELF_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(ms));
    if (n <= 0) {
        printf("  [UART 自检·%s] 收到 0 字节\n", why);
        return 0;
    }

    int m = (n > 24) ? 24 : n;
    char hex[3 * 25 + 1];
    for (int i = 0; i < m; i++) {
        snprintf(&hex[i * 3], 4, "%02X ", buf[i]);
    }
    hex[m * 3] = 0;

    char asc[25];
    for (int i = 0; i < m; i++) {
        asc[i] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
    }
    asc[m] = 0;

    printf("  [UART 自检·%s] 收到 %d 字节\n", why, n);
    printf("      hex: %s\n", hex);
    printf("      asc: |%s|\n", asc);
    return n;
}

/* ---------------------------------------------------------------------------
 * 一个完整自检周期：关断 → 监听（应为 0）→ 打开 → 监听（应有字节）
 * ------------------------------------------------------------------------- */

static void run_selfcheck_cycle(const char *stage)
{
    printf("\n============================================================\n");
    printf(" 自检周期：%s\n", stage);
    printf("============================================================\n");

    /* --- 关断 --- */
    edk60_radar_power_off();
    vTaskDelay(pdMS_TO_TICKS(400));
    print_state("关断后");
    int n_off = self_uart_listen(1200, "关断状态");

    /* --- 打开 --- */
    edk60_radar_power_on();
    vTaskDelay(pdMS_TO_TICKS(1500));   /* 等模组上电初始化（radar_ready 约 1s） */
    print_state("使能后");
    int n_on = self_uart_listen(2500, "使能状态");

    /* --- 判读 --- */
    printf("\n[判读]\n");
    if (n_off == 0 && n_on > 0) {
        printf("  ✅ 符合预期：关断时无数据、使能后有数据\n");
        printf("     ⇒ 电源控制（IO%d）与 UART 通路（RX=IO%d）均正常\n",
               (int)EDK60_PIN_RADAR_PWR_EN, (int)SELF_RX_PIN);
    } else if (n_off == 0 && n_on == 0) {
        printf("  ⚠ 两个状态都没有数据。请依次确认：\n");
        printf("     1) 模组是否插在 H3（不是 H4）且方向正确；\n");
        printf("     2) 丝印 \"RADAR\" 的红灯是否亮（不亮 = 3.3V_RADAR 没建立）；\n");
        printf("     3) 模组射频默认关闭：只上电时通常仍会主动发 radar_ready/get_time；\n");
        printf("        若完全静默，请改用 demo 5.1 radar_bridge（会自动发 turn_on）。\n");
    } else if (n_off > 0) {
        printf("  ⚠ 关断状态下仍收到数据，可能是：\n");
        printf("     · 3.3V_RADAR 没被真正切断（用万用表量 H3 的 V/P 脚）；\n");
        printf("     · 或收到的是悬空噪声（看上面 hex 是否像合法帧 0x5A 开头）。\n");
    }
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */

void demo_radar_power_start(void)
{
    ESP_LOGI(TAG, "=== radar_power：雷达电源控制与链路自检 ===");
    ESP_LOGI(TAG, "实测结论：EN=IO%d（电源） RX=IO%d（模组TX） TX=IO%d（模组RX） 橙灯=IO%d",
             (int)EDK60_PIN_RADAR_PWR_EN, (int)SELF_RX_PIN,
             (int)SELF_TX_PIN, (int)EDK60_PIN_RADAR_LED);
    ESP_LOGI(TAG, "模组请插在 H3；H4 的 T 与 EN 同网络，插 H4 会电源/数据冲突");

    /* 1) 电源脚初始化（初始化后默认关断，对热插拔安全） */
    ESP_ERROR_CHECK(edk60_radar_power_init());

    /* 2) 橙色指示灯单独驱动（它只是指示灯，不参与电源控制） */
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << EDK60_PIN_RADAR_LED,
        .mode        = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en  = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type   = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(EDK60_PIN_RADAR_LED, 1);   /* 高电平点亮 */

    print_state("初始化完成（电源默认关断）");

    /* 3) UART 自检通路 */
    bool uart_ok = self_uart_install();

    /* 4) 跑一次完整自检 */
    if (uart_ok) {
        run_selfcheck_cycle("上电自动执行");
    } else {
        ESP_LOGW(TAG, "UART 不可用，跳过数据自检，只做电源通断演示");
    }

    /* 5) 结束后保持上电（便于肉眼观察红灯，也方便接着用 radar_bridge） */
    edk60_radar_power_on();
    vTaskDelay(pdMS_TO_TICKS(300));
    print_state("自检结束（保持上电）");

    printf("\n[操作说明] 按一下 BOOT 键（IO%d）可切换雷达电源通断，每次都会打印状态。\n",
           (int)EDK60_PIN_BUTTON_BOOT);
    printf("           要拿心率/呼吸率等体征数据，请用 demo 5.1 radar_bridge。\n");

    /* 6) 按键轮询：切换电源并打印 */
    edk60_button_init_polling();
    bool last_pressed = false;
    bool power_on = true;

    while (true) {
        bool pressed = edk60_button_is_pressed();
        if (pressed && !last_pressed) {
            power_on = !power_on;
            if (power_on) {
                edk60_radar_power_on();
            } else {
                edk60_radar_power_off();
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            printf("\n>>> BOOT 键切换：雷达电源 → %s\n", power_on ? "开" : "关");
            print_state("按键切换后");
            printf("  （丝印 RADAR 的红灯应%s）\n", power_on ? "亮" : "灭");

            if (uart_ok) {
                self_uart_listen(1500, power_on ? "按键开电后" : "按键关电后");
            }
        }
        last_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
