/*
 * EDK60 板级支持包 —— GPIO / I2C / 雷达电源 / 控制台
 *
 * 所有 GPIO 编号来自 docs/SCH_EDK60_VA.pdf（EDK60_VA V1.0）。
 * 引脚对照表见 docs/EDK60-pinmap.md。
 */

#include "edk60_board.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "edk60";

/* ---------------------------------------------------------------------------
 * 编译期校验：把「引脚编号被误改」这类问题在编译阶段拦下来
 * ------------------------------------------------------------------------- */
_Static_assert(EDK60_PIN_LED_RX == 8, "LED RX 必须为 IO8（原理图 R6/LED2）");
_Static_assert(EDK60_PIN_LED_TX == 9, "LED TX 必须为 IO9（原理图 R7/LED3）");
_Static_assert(EDK60_PIN_I2C_SDA == 6, "SDA 必须为 IO6（原理图 H1 网络标号 SDA/IO6）");
_Static_assert(EDK60_PIN_I2C_SCL == 7, "SCL 必须为 IO7（原理图 H1 网络标号 SCL/IO7）");
_Static_assert(EDK60_PIN_RADAR_UART_RX == 4, "雷达 UART RX 已实测为 IO4（模组插 H3 时的 T 脚）");
_Static_assert(EDK60_PIN_RADAR_PWR_EN == 14, "雷达电源使能已实测为 IO14（U4 的 EN，拉高上电）");
_Static_assert(EDK60_PIN_RADAR_LED == 15, "雷达指示橙灯已实测为 IO15，与电源 EN 无关");
_Static_assert(EDK60_PIN_D0_RX == 16 && EDK60_PIN_D1_TX == 17, "H2 的 D0/D1 必须为 IO16/IO17");

/* LED 表：索引 0 = 丝印 RX(LED2, IO8)，索引 1 = 丝印 TX(LED3, IO9) */
static const gpio_num_t s_led_pins[EDK60_LED_GPIO_COUNT] = {
    EDK60_PIN_LED_RX,
    EDK60_PIN_LED_TX,
};

static SemaphoreHandle_t s_led_lock;

/* ---------------------------------------------------------------------------
 * 横幅 / 引脚表 / 复位原因
 * ------------------------------------------------------------------------- */

void edk60_print_banner(const char *demo_name)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    const esp_app_desc_t *app = esp_app_get_description();

    printf("\n");
    printf("==========================================================\n");
    printf("  EDK60 (ESP32-C6)  Demo 运行中\n");
    printf("----------------------------------------------------------\n");
    printf("  板卡      : %s  [%s]\n", EDK60_BOARD_NAME, EDK60_BOARD_REV);
    printf("  模组      : %s\n", EDK60_MODULE_NAME);
    printf("  芯片      : %s rev v%d.%d, %d 核 @ %d MHz\n",
           CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
           chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    printf("  Flash     : %u bytes (%.0f MB)，片上无 PSRAM\n",
           (unsigned)flash_size, (double)flash_size / (1024.0 * 1024.0));
    printf("  IDF       : v%s\n", esp_get_idf_version());
    printf("  App       : %s (%s)\n",
           app ? app->version : "?", app ? app->project_name : "?");
    printf("  当前 Demo : %s\n", demo_name ? demo_name : "-");
    printf("  基站 MAC  : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("==========================================================\n");
}

void edk60_print_pinmap(void)
{
    printf("\n[EDK60 引脚映射]（来源：SCH_EDK60_VA.pdf / EDK60_VA V1.0）\n");
    printf("  LED  RX  丝印 RX   : IO%-2d  经 330R 到 LED，低电平点亮（LED2）\n", EDK60_PIN_LED_RX);
    printf("  LED  TX  丝印 TX   : IO%-2d  经 330R 到 LED，低电平点亮（LED3）\n", EDK60_PIN_LED_TX);
    printf("  按键 BOOT          : IO%-2d  低电平按下（同时是 strapping 脚，上电勿按住）\n", EDK60_PIN_BUTTON_BOOT);
    printf("  雷达 UART RX       : IO%-2d  模组 TX -> MCU（H3 的 T 脚）\n", EDK60_PIN_RADAR_UART_RX);
    printf("  雷达 UART TX       : IO%-2d  MCU -> 模组 RX（H3 的 A 脚），发命令走这根\n", EDK60_PIN_RADAR_UART_TX);
    printf("  雷达电源使能 EN    : IO%-2d  U4.EN；输出高=使能 3.3V_RADAR（实测确认）\n", EDK60_PIN_RADAR_PWR_EN);
    printf("  雷达指示橙灯(丝印L): IO%-2d  仅指示灯，与电源 EN 无关（实测确认）\n", EDK60_PIN_RADAR_LED);
    printf("  雷达模拟输入       : IO2 / IO3（IO4/IO5 已被 UART 占用，勿同时用）\n");
    printf("  USB D- / D+        : IO12 / IO13  原生 USB Serial/JTAG\n");
    printf("  I2C  SDA / SCL     : IO%d / IO%d  H1\n", EDK60_PIN_I2C_SDA, EDK60_PIN_I2C_SCL);
    printf("  数字口 H2          : D0/RX=IO16  D1/TX=IO17  D2=IO18  D3=IO19\n");
    printf("                       D4=IO20  D5=IO21  D6=IO22  D7=IO23\n");
    printf("  数字口 H1          : D8=IO8  D9=IO9  D10=IO2  D11=IO3  D12=IO4  D13=IO5\n");
    printf("  模拟口 H3/H6       : A0=IO0  A1=IO1  A2=IO2  A3=IO3  A4=IO4  A5=IO5（均为 ADC1）\n");
    printf("  ADC1 通道          : IO0=CH0  IO1=CH1  IO2=CH2  IO3=CH3  IO4=CH4  IO5=CH5  IO6=CH6\n");
    printf("  常亮指示灯         : LED1=ON(绿,3.3V)  ——  其余受控：IO8/IO9/IO15 共 3 颗\n");
    printf("  RTC/深睡唤醒脚     : 仅 IO0..IO7（EXT1）；IO9 不支持 ext1\n");
    printf("\n");
}

void edk60_print_reset_reason(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    const char *name = "UNKNOWN";

    switch (r) {
    case ESP_RST_UNKNOWN:   name = "UNKNOWN(未知)"; break;
    case ESP_RST_POWERON:   name = "POWERON(上电复位)"; break;
    case ESP_RST_EXT:       name = "EXT(外部复位脚)"; break;
    case ESP_RST_SW:        name = "SW(软件复位)"; break;
    case ESP_RST_PANIC:     name = "PANIC(异常/看门狗)"; break;
    case ESP_RST_INT_WDT:   name = "INT_WDT(中断看门狗)"; break;
    case ESP_RST_TASK_WDT:  name = "TASK_WDT(任务看门狗)"; break;
    case ESP_RST_WDT:       name = "WDT(其他看门狗)"; break;
    case ESP_RST_DEEPSLEEP: name = "DEEPSLEEP(深睡唤醒)"; break;
    case ESP_RST_BROWNOUT:  name = "BROWNOUT(电压不足)"; break;
    case ESP_RST_SDIO:      name = "SDIO"; break;
    default: break;
    }

    printf("[复位原因] %s (%d)\n", name, (int)r);

    if (r == ESP_RST_DEEPSLEEP) {
        uint32_t causes = esp_sleep_get_wakeup_causes();
        if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
            printf("[深睡唤醒] TIMER（定时器）\n");
        }
#if SOC_PM_SUPPORT_EXT1_WAKEUP
        if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
            uint64_t status = esp_sleep_get_ext1_wakeup_status();
            printf("[深睡唤醒] EXT1，触发脚掩码 = 0x%08llX\n", (unsigned long long)status);
        }
#endif
        if (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) {
            uint64_t status = esp_sleep_get_gpio_wakeup_status();
            printf("[深睡唤醒] GPIO，触发脚掩码 = 0x%08llX\n", (unsigned long long)status);
        }
    }
}

/* ---------------------------------------------------------------------------
 * LED
 * ------------------------------------------------------------------------- */

esp_err_t edk60_led_init(void)
{
    if (s_led_lock == NULL) {
        s_led_lock = xSemaphoreCreateMutex();
        if (s_led_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    for (int i = 0; i < EDK60_LED_GPIO_COUNT; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_led_pins[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LED IO%d 配置失败: %s", s_led_pins[i], esp_err_to_name(err));
            return err;
        }
        gpio_set_level(s_led_pins[i], !EDK60_LED_ACTIVE_LEVEL);  /* 初始灭 */
    }
    ESP_LOGD(TAG, "LED 已初始化: IO%d, IO%d", s_led_pins[0], s_led_pins[1]);
    return ESP_OK;
}

gpio_num_t edk60_led_pin(int led_index)
{
    if (led_index < 0 || led_index >= EDK60_LED_GPIO_COUNT) {
        return GPIO_NUM_NC;
    }
    return s_led_pins[led_index];
}

esp_err_t edk60_led_set(int led_index, bool on)
{
    if (led_index < 0 || led_index >= EDK60_LED_GPIO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    int level = on ? EDK60_LED_ACTIVE_LEVEL : !EDK60_LED_ACTIVE_LEVEL;
    return gpio_set_level(s_led_pins[led_index], level);
}

esp_err_t edk60_led_toggle(int led_index)
{
    if (led_index < 0 || led_index >= EDK60_LED_GPIO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    int level = gpio_get_level(s_led_pins[led_index]);
    return gpio_set_level(s_led_pins[led_index], !level);
}

esp_err_t edk60_led_set_all(bool on)
{
    esp_err_t last = ESP_OK;
    for (int i = 0; i < EDK60_LED_GPIO_COUNT; i++) {
        esp_err_t err = edk60_led_set(i, on);
        if (err != ESP_OK) {
            last = err;
        }
    }
    return last;
}

/* ---------------------------------------------------------------------------
 * 按键 BOOT（IO9，低有效）
 * ------------------------------------------------------------------------- */

static uint32_t s_btn_press_tick;               /* 记录按下时刻，用于计算按住时长 */
static edk60_button_cb_t s_btn_cb;
static void *s_btn_cb_ctx;
static QueueHandle_t s_btn_evt_queue;
static TaskHandle_t s_btn_task;

/* 消抖窗口与重触发抑制窗口（毫秒） */
#define EDK60_BTN_DEBOUNCE_MS   30
#define EDK60_BTN_REPEAT_MS     200

typedef struct {
    uint32_t level;  /* 触发时的原始电平 */
} btn_evt_t;

static void IRAM_ATTR edk60_button_isr(void *arg)
{
    btn_evt_t evt = {.level = (uint32_t)gpio_get_level(EDK60_PIN_BUTTON_BOOT)};
    BaseType_t hp_task_woken = pdFALSE;
    if (s_btn_evt_queue) {
        xQueueSendFromISR(s_btn_evt_queue, &evt, &hp_task_woken);
    }
    if (hp_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void edk60_button_task(void *arg)
{
    (void)arg;
    btn_evt_t evt;
    int stable = gpio_get_level(EDK60_PIN_BUTTON_BOOT);

    /* 与 ISR 约定：ISR 只投递「电平变化候选」，这里做时间窗校验（软件消抖） */
    /* ISR 侧不可调用 vTaskDelay，因此等待与去抖全部在本任务中完成。 */
    bool pending = false;
    TickType_t first_edge = 0;
    int pending_level = stable;

    while (1) {
        if (!pending) {
            if (xQueueReceive(s_btn_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
                pending = true;
                first_edge = xTaskGetTickCount();
                pending_level = (int)evt.level;
            }
            continue;
        }

        if (xQueueReceive(s_btn_evt_queue, &evt, pdMS_TO_TICKS(EDK60_BTN_DEBOUNCE_MS)) == pdTRUE) {
            /* 抖动，刷新计时并沿用最新电平 */
            first_edge = xTaskGetTickCount();
            pending_level = (int)evt.level;
            continue;
        }

        if ((xTaskGetTickCount() - first_edge) < pdMS_TO_TICKS(EDK60_BTN_DEBOUNCE_MS)) {
            continue;
        }

        pending = false;
        int now = gpio_get_level(EDK60_PIN_BUTTON_BOOT);
        if (now != pending_level) {
            continue;  /* 判定期间又变了，丢弃 */
        }
        if (now == stable) {
            continue;  /* 无变化（例如被 vTaskDelay 打断后的重入），忽略 */
        }
        stable = now;

        uint32_t hold_ms = 0;
        edk60_button_event_t event;
        if (now == EDK60_BUTTON_ACTIVE_LEVEL) {
            event = EDK60_BUTTON_EVENT_PRESSED;
            s_btn_press_tick = xTaskGetTickCount();
        } else {
            event = EDK60_BUTTON_EVENT_RELEASED;
            hold_ms = (xTaskGetTickCount() - s_btn_press_tick) * portTICK_PERIOD_MS;
        }

        ESP_LOGI(TAG, "按键事件: %s%s", event == EDK60_BUTTON_EVENT_PRESSED ? "按下" : "松开",
                 hold_ms ? "" : "");
        if (s_btn_cb) {
            s_btn_cb(event, hold_ms, s_btn_cb_ctx);
        }
    }
}

static esp_err_t edk60_button_gpio_config(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << EDK60_PIN_BUTTON_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    return gpio_config(&cfg);
}

esp_err_t edk60_button_init_polling(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << EDK60_PIN_BUTTON_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "按键已初始化（轮询模式），IO%d", EDK60_PIN_BUTTON_BOOT);
    }
    return err;
}

bool edk60_button_is_pressed(void)
{
    return gpio_get_level(EDK60_PIN_BUTTON_BOOT) == EDK60_BUTTON_ACTIVE_LEVEL;
}

esp_err_t edk60_button_init_isr(edk60_button_cb_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_btn_task == NULL) {
        s_btn_evt_queue = xQueueCreate(16, sizeof(btn_evt_t));
        if (s_btn_evt_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
        xTaskCreate(edk60_button_task, "edk60_btn", 4096, NULL, 5, &s_btn_task);
        if (s_btn_task == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }

    s_btn_cb = cb;
    s_btn_cb_ctx = user_ctx;

    esp_err_t err = edk60_button_gpio_config();
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_isr_handler_add(EDK60_PIN_BUTTON_BOOT, edk60_button_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_intr_enable(EDK60_PIN_BUTTON_BOOT);
}

/* ---------------------------------------------------------------------------
 * I2C 主机总线（SDA=IO6 / SCL=IO7）
 * ------------------------------------------------------------------------- */

static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t edk60_i2c_init(edk60_i2c_bus_handle_t *out_bus)
{
    if (out_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i2c_bus != NULL) {
        *out_bus = s_i2c_bus;
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,                     /* 自动选择端口 */
        .sda_io_num = EDK60_PIN_I2C_SDA,
        .scl_io_num = EDK60_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线创建失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C 主机总线就绪: SDA=IO%d, SCL=IO%d, %d Hz",
             EDK60_PIN_I2C_SDA, EDK60_PIN_I2C_SCL, EDK60_I2C_DEFAULT_FREQ_HZ);
    *out_bus = s_i2c_bus;
    return ESP_OK;
}

esp_err_t edk60_i2c_probe(edk60_i2c_bus_handle_t bus, uint8_t addr_7bit, bool *out_present)
{
    if (bus == NULL || out_present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_master_probe((i2c_master_bus_handle_t)bus, addr_7bit, 50);
    *out_present = (err == ESP_OK);
    return ESP_OK;
}

esp_err_t edk60_i2c_deinit(edk60_i2c_bus_handle_t bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus != s_i2c_bus) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_del_master_bus(s_i2c_bus);
    s_i2c_bus = NULL;
    return err;
}

/* ---------------------------------------------------------------------------
 * 雷达电源（IO15 -> U4.EN，10k 下拉到 GND；高电平 = 使能 3.3V_RADAR）
 *
 * 硬件事实（2026-10-14 逐层读图核实，勿凭印象改写）：
 *   链路 1（使能）： U4.EN ── R2(10k) ── GND，且 EN 接 IO15（网络名 RADAR_PWR_EN）
 *   链路 2（指示）： IO15 ── R9(330Ω) ── LED4(橙色, 丝印 "L") ── GND
 *
 *   推论：
 *     - 复位后 IO15 默认高阻，R2 把 EN 拉低 ⇒ **雷达电源默认关闭**（对插拔模组安全）；
 *     - 使能 = IO15 推挽输出高（10k 下拉只需 0.33mA，无需开漏）；
 *     - 关闭 = IO15 输出低电平 / 高阻（LED4 同时熄灭，不产生额外静态电流）；
 *     - 丝印 "L" 的橙灯就是雷达电源指示灯，可肉眼校验本驱动的每一次切换。
 * ------------------------------------------------------------------------- */

esp_err_t edk60_radar_power_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << EDK60_PIN_RADAR_PWR_EN,
        /* ⚠ 必须用 INPUT_OUTPUT，不能只配成 OUTPUT。
         * 只配纯 OUTPUT 时输入通道是关闭的，gpio_get_level() 在 ESP32-C6 上
         * **永远读回 0**，于是 edk60_radar_power_is_on() 会误报「雷达仍关闭」。
         * 本工程 2026-10-15 实机踩到：使能后日志打印 "EN=低(仍关闭)"，
         * 而逐脚直测证明 IO15 实际能正常输出高（电源其实是开着的）。
         * 使能输入通道后回读才反映引脚真实电平。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   /* 外部已有 10k 下拉到 GND */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "雷达电源控制脚 IO%d 初始化失败: %s",
                 EDK60_PIN_RADAR_PWR_EN, esp_err_to_name(err));
        return err;
    }
    /* 初始状态 = 关闭（与硬件默认一致，避免初始化瞬间给雷达上电） */
    err = gpio_set_level(EDK60_PIN_RADAR_PWR_EN, !EDK60_RADAR_PWR_EN_ACTIVE_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "雷达电源控制脚 IO%d 就绪（10k 下拉，默认关闭；输出高=使能，LED4 同步点亮）",
             EDK60_PIN_RADAR_PWR_EN);
    return ESP_OK;
}

esp_err_t edk60_radar_power_on(void)
{
    /* 输出高 = EN 被拉到 3.3V（远高于 AP7362 的 EN 高门限）⇒ 3.3V_RADAR 输出，LED4 亮 */
    return gpio_set_level(EDK60_PIN_RADAR_PWR_EN, EDK60_RADAR_PWR_EN_ACTIVE_LEVEL);
}

esp_err_t edk60_radar_power_off(void)
{
    /* 输出低 = EN 被 0Ω 强下拉到 GND ⇒ 3.3V_RADAR 关断，LED4 同步熄灭 */
    return gpio_set_level(EDK60_PIN_RADAR_PWR_EN, !EDK60_RADAR_PWR_EN_ACTIVE_LEVEL);
}

bool edk60_radar_power_is_on(void)
{
    /* 推挽输出，直接读回 IO15 的实际电平即代表 EN 状态 */
    return gpio_get_level(EDK60_PIN_RADAR_PWR_EN) == EDK60_RADAR_PWR_EN_ACTIVE_LEVEL;
}

/* ---------------------------------------------------------------------------
 * 控制台
 * ------------------------------------------------------------------------- */

esp_err_t edk60_console_install_usb_serial_jtag(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* 主控制台就是 USB Serial/JTAG：driver_install 会接管 RX 提供可读 stdin */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB Serial/JTAG 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB Serial/JTAG 已就绪（IO12=D-, IO13=D+），可收发");
    return ESP_OK;
#else
    /* 主控制台是 UART0（H2 的 D0/RX=IO16、D1/TX=IO17），stdin 由 UART 驱动提供，
     * USB Serial/JTAG 仅作为只读的第二输出口，无需额外安装驱动。 */
    ESP_LOGI(TAG, "主控制台 = UART0（IO16/IO17）；USB(IO12/IO13) 为只读第二输出口");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
