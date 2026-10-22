/*
 * EDK60 Demo 1.3 —— button
 *
 * 目的：验证 BOOT 按键（SW2，IO9，低电平按下）的输入通路，
 *       同时对比「轮询」与「中断 + 软件消抖」两种读键方式。
 *
 * 硬件事实（原理图 SW2 = TS-1088-AR020016）：
 *   IO9 --- SW2 --- GND，另有 4.7k 上拉到 3.3V。
 *   IO9 是 C6 的 BOOT strapping 脚：上电/复位瞬间被按住会进入下载模式，
 *   因此「上电时按住按键」不是 bug 而是板子的设计行为。
 *
 * 演示内容：
 *   1) 中断方式：按下/松开都会打印，并给出按住时长；
 *      按住 >= 2 秒判定为「长按」，额外打印提示；
 *   2) 轮询方式：每 100ms 采样一次，仅在状态变化时打印，同时给出轮询延迟的上界；
 *   3) 计数统计：每 10 次按键/释放打印一次合计，便于长时间老化观察。
 */

#include <inttypes.h>
#include <stdio.h>

#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo_button";

#define LONG_PRESS_MS 2000

static volatile uint32_t s_press_count;
static volatile uint32_t s_release_count;

static void on_button_event(edk60_button_event_t event, uint32_t hold_ms, void *user_ctx)
{
    (void)user_ctx;

    if (event == EDK60_BUTTON_EVENT_PRESSED) {
        s_press_count++;
        printf("[按键/中断] 按下   (累计第 %" PRIu32 " 次)\n", s_press_count);
    } else {
        s_release_count++;
        printf("[按键/中断] 松开   按住 %" PRIu32 " ms%s\n", hold_ms,
               (hold_ms >= LONG_PRESS_MS) ? "  <-- 长按（>= 2s）" : "");
    }

    if (s_press_count % 10 == 0 && event == EDK60_BUTTON_EVENT_PRESSED) {
        printf("[按键/统计] 已按下 %" PRIu32 " 次，松开 %" PRIu32 " 次\n",
               s_press_count, s_release_count);
    }
}

/* 轮询任务：与中断方式互为对照，用来观察「慢采样会不会漏掉短按」 */
static void poll_task(void *arg)
{
    (void)arg;
    bool last = edk60_button_is_pressed();
    uint32_t poll_count = 0;

    while (1) {
        bool now = edk60_button_is_pressed();
        if (now != last) {
            ESP_LOGI(TAG, "[轮询/100ms] 状态变化 -> %s", now ? "按下" : "松开");
            last = now;
        }
        poll_count++;
        if (poll_count % 600 == 0) {
            ESP_LOGI(TAG, "[轮询] 已运行约 %" PRIu32 " 秒（%s）",
                     poll_count / 10U, edk60_button_is_pressed() ? "当前按下" : "当前松开");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void demo_button_start(void)
{
    ESP_LOGI(TAG, "=== button：按键输入验证（BOOT, IO%d, 低有效）===", EDK60_PIN_BUTTON_BOOT);

    ESP_ERROR_CHECK(edk60_led_init());
    ESP_ERROR_CHECK(edk60_button_init_polling());

    /* 当前电平，用来确认上电时按键没有被误按住 */
    ESP_LOGI(TAG, "IO%d 当前电平 = %d（0=按下/低，1=松开/高）",
             EDK60_PIN_BUTTON_BOOT, (int)gpio_get_level(EDK60_PIN_BUTTON_BOOT));

    ESP_ERROR_CHECK(edk60_button_init_isr(on_button_event, NULL));
    ESP_LOGI(TAG, "中断已注册（含 30ms 软件消抖）。请按 BOOT 键观察输出。");

    xTaskCreate(poll_task, "poll", 3072, NULL, 3, NULL);
}


