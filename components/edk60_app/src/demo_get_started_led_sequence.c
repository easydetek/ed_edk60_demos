/*
 * EDK60 Demo 3.2 —— led_sequence
 *
 * 目的：用 LEDC 硬件 PWM 演示 LED 的亮度控制与灯效编排。
 *       blink（1.2）验证的是 GPIO 通断，本 demo 验证的是「可调亮度 + 定时编排」。
 *
 * 硬件事实（原理图 R6/R7 + LED2/LED3）：
 *   IO8 -> 330R -> LED2(黄,丝印 RX) -> GND
 *   IO9 -> 330R -> LED3(黄,丝印 TX) -> GND
 *   低电平点亮 ⇒ LEDC duty=0 时最亮、duty=满量程时最暗（亮度值要做反相）。
 *   两颗灯都是普通单色 LED（非 RGB），只能调亮暗，不能调颜色。
 *
 * 灯效列表（用 BOOT 键 IO9 循环切换）：
 *   0. 交替闪      RX/TX 交替亮灭（1Hz）
 *   1. 呼吸灯      两颗同步渐亮渐暗（硬件 fade）
 *   2. 追光        RX/TX 快速交替（5Hz）
 *   3. 呼吸追光    一颗渐亮同时另一颗渐暗（交叉渐变）
 *   4. SOS         摩尔斯电码 ...---...（国际求救信号，便于直观辨认）
 *   5. 全灭        停 PWM，LED 熄灭
 *
 * 已知硬件共用：BOOT 按键与 LED_TX 共用 IO9，因此 LED_TX 被 PWM 驱动时
 * 按键采样可能不稳定。本 demo 明确提示该现象，不假装它不存在。
 *
 * 观察点：
 *   - 呼吸/渐变动作为硬件 PWM 渐变，肉眼应无台阶感；
 *   - 丝印 ON(绿)、L(橙) 两颗灯不受程序控制，始终常亮，可作对照；
 *   - 每次切换灯效，日志会给出现用灯效编号与名称。
 */

#include <stdio.h>

#include "driver/ledc.h"
#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo_ledseq";

/* ---- PWM 配置 ---- */
#define LED_PWM_FREQ_HZ     5000                 /* 5kHz：远高于闪烁感知频率，无频闪 */
#define LED_PWM_RES         LEDC_TIMER_8_BIT      /* 8bit = 256 级亮度 */
#define LED_PWM_MAX_DUTY    ((1 << 8) - 1)        /* 255 */
#define LED_PWM_MODE        LEDC_LOW_SPEED_MODE   /* C6 只有低速模式 */
#define LED_PWM_TIMER       LEDC_TIMER_0

typedef enum {
    FX_ALTERNATE = 0,
    FX_BREATH,
    FX_CHASE,
    FX_CROSS_BREATH,
    FX_SOS,
    FX_ALL_OFF,
    FX_COUNT
} led_fx_t;

static const char *s_fx_names[FX_COUNT] = {
    "交替闪 (1Hz)",
    "呼吸灯 (同步渐变)",
    "追光 (5Hz 交替)",
    "呼吸追光 (交叉渐变)",
    "SOS (摩尔斯电码)",
    "全灭",
};

static ledc_channel_t s_chan[EDK60_LED_GPIO_COUNT];
static volatile int s_fx = FX_ALTERNATE;

/* ---------------------------------------------------------------------------
 * PWM 基础
 * ------------------------------------------------------------------------- */

static esp_err_t led_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LED_PWM_MODE,
        .duty_resolution = LED_PWM_RES,
        .timer_num = LED_PWM_TIMER,
        .freq_hz = LED_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC 定时器配置失败: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < EDK60_LED_GPIO_COUNT; i++) {
        s_chan[i] = (ledc_channel_t)(LEDC_CHANNEL_0 + i);
        ledc_channel_config_t ch = {
            .gpio_num = edk60_led_pin(i),
            .speed_mode = LED_PWM_MODE,
            .channel = s_chan[i],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LED_PWM_TIMER,
            .duty = LED_PWM_MAX_DUTY,   /* 低电平点亮 ⇒ duty 拉满 = 灭 */
            .hpoint = 0,
            .flags.output_invert = 0,
        };
        err = ledc_channel_config(&ch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LEDC 通道 %d(IO%d) 配置失败: %s",
                     i, edk60_led_pin(i), esp_err_to_name(err));
            return err;
        }
    }

    err = ledc_fade_func_install(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "LEDC 渐变功能不可用（%s），呼吸灯将退化为直接设定亮度", esp_err_to_name(err));
    }
    return ESP_OK;
}

/** brightness: 0.0 = 灭, 1.0 = 最亮（内部对「低电平点亮」做反相） */
static void led_set_brightness(int led_index, float brightness)
{
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    uint32_t duty = (uint32_t)((1.0f - brightness) * LED_PWM_MAX_DUTY + 0.5f);
    ledc_set_duty(LED_PWM_MODE, s_chan[led_index], duty);
    ledc_update_duty(LED_PWM_MODE, s_chan[led_index]);
}

static void led_fade_to(int led_index, float brightness, int ms)
{
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    uint32_t duty = (uint32_t)((1.0f - brightness) * LED_PWM_MAX_DUTY + 0.5f);
    esp_err_t err = ledc_set_fade_time_and_start(LED_PWM_MODE, s_chan[led_index],
                                                 duty, (uint32_t)ms, LEDC_FADE_NO_WAIT);
    if (err != ESP_OK) {
        led_set_brightness(led_index, brightness);
    }
}

static void all_off(void)
{
    for (int i = 0; i < EDK60_LED_GPIO_COUNT; i++) {
        led_set_brightness(i, 0.0f);
    }
}

/* ---------------------------------------------------------------------------
 * 灯效
 * ------------------------------------------------------------------------- */

static void fx_alternate(void)
{
    led_set_brightness(0, 1.0f);
    led_set_brightness(1, 0.0f);
    vTaskDelay(pdMS_TO_TICKS(500));
    led_set_brightness(0, 0.0f);
    led_set_brightness(1, 1.0f);
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void fx_breath(void)
{
    const int half_ms = 1200;
    led_fade_to(0, 1.0f, half_ms);
    led_fade_to(1, 1.0f, half_ms);
    vTaskDelay(pdMS_TO_TICKS(half_ms + 30));
    led_fade_to(0, 0.0f, half_ms);
    led_fade_to(1, 0.0f, half_ms);
    vTaskDelay(pdMS_TO_TICKS(half_ms + 30));
}

static void fx_chase(void)
{
    led_set_brightness(0, 1.0f);
    led_set_brightness(1, 0.0f);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_set_brightness(0, 0.0f);
    led_set_brightness(1, 1.0f);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void fx_cross_breath(void)
{
    const int half_ms = 1000;
    led_fade_to(0, 1.0f, half_ms);
    led_fade_to(1, 0.0f, half_ms);
    vTaskDelay(pdMS_TO_TICKS(half_ms + 30));
    led_fade_to(0, 0.0f, half_ms);
    led_fade_to(1, 1.0f, half_ms);
    vTaskDelay(pdMS_TO_TICKS(half_ms + 30));
}

/* 两颗灯同时闪：短点 200ms、长划 600ms */
static void sos_flash(int ms)
{
    led_set_brightness(0, 1.0f);
    led_set_brightness(1, 1.0f);
    vTaskDelay(pdMS_TO_TICKS(ms));
    led_set_brightness(0, 0.0f);
    led_set_brightness(1, 0.0f);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void fx_sos(void)
{
    const int dot = 200, dash = 600;
    for (int i = 0; i < 3; i++) sos_flash(dot);      /* S: ... */
    vTaskDelay(pdMS_TO_TICKS(400));
    for (int i = 0; i < 3; i++) sos_flash(dash);     /* O: --- */
    vTaskDelay(pdMS_TO_TICKS(400));
    for (int i = 0; i < 3; i++) sos_flash(dot);      /* S: ... */
    vTaskDelay(pdMS_TO_TICKS(1800));
}

/* ---------------------------------------------------------------------------
 * 按键切换灯效
 * ------------------------------------------------------------------------- */

static void on_button(edk60_button_event_t event, uint32_t hold_ms, void *ctx)
{
    (void)hold_ms;
    (void)ctx;
    if (event != EDK60_BUTTON_EVENT_PRESSED) {
        return;
    }
    s_fx = (s_fx + 1) % FX_COUNT;
    ESP_LOGI(TAG, ">>> 切换到灯效 %d: %s", s_fx, s_fx_names[s_fx]);
}

/* ---------------------------------------------------------------------------
 * 灯效任务
 * ------------------------------------------------------------------------- */

static void fx_task(void *arg)
{
    (void)arg;
    int last = -1;

    while (1) {
        int fx = s_fx;
        if (fx != last) {
            ESP_LOGI(TAG, "开始播放灯效 %d: %s", fx, s_fx_names[fx]);
            all_off();
            last = fx;
        }

        switch (fx) {
        case FX_ALTERNATE:    fx_alternate();     break;
        case FX_BREATH:       fx_breath();        break;
        case FX_CHASE:        fx_chase();         break;
        case FX_CROSS_BREATH: fx_cross_breath();  break;
        case FX_SOS:          fx_sos();           break;
        case FX_ALL_OFF:
        default:              all_off(); vTaskDelay(pdMS_TO_TICKS(200)); break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */

void demo_led_sequence_start(void)
{
    ESP_LOGI(TAG, "=== led_sequence：LEDC PWM 灯效编排 ===");
    ESP_LOGI(TAG, "LED 丝印 RX -> IO%d，丝印 TX -> IO%d（低电平点亮，亮度 %d 级）",
             edk60_led_pin(0), edk60_led_pin(1), LED_PWM_MAX_DUTY);
    ESP_LOGI(TAG, "PWM: %d Hz / 定时器 %d / 通道 %d,%d",
             LED_PWM_FREQ_HZ, LED_PWM_TIMER, LEDC_CHANNEL_0, LEDC_CHANNEL_1);
    ESP_LOGI(TAG, "按 BOOT 键(IO%d) 循环切换 %d 种灯效", EDK60_PIN_BUTTON_BOOT, FX_COUNT);
    ESP_LOGI(TAG, "提示：BOOT 键与 LED_TX 共用 IO%d，PWM 驱动期间按键采样可能不稳定，"
                  "属本板已知的引脚共用现象。", EDK60_PIN_BUTTON_BOOT);

    ESP_ERROR_CHECK(led_pwm_init());
    ESP_ERROR_CHECK(edk60_button_init_isr(on_button, NULL));

    xTaskCreate(fx_task, "led_fx", 4096, NULL, 4, NULL);
}
