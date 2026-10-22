/*
 * EDK60 Demo 1.2 —— blink
 *
 * 目的：最小 GPIO 输出验证。两颗可控 LED（丝印 RX 与 TX）以不同节奏闪烁，
 *       用来确认程序在跑、GPIO 输出正常、且两颗灯是相互独立的。
 *
 * 硬件事实（原理图 R6/R7 + LED2/LED3）：
 *   IO8 -> 330R -> LED2(黄) -> GND
 *   IO9 -> 330R -> LED3(黄) -> GND
 *   低电平点亮，高电平熄灭。
 *   注意 IO9 同时是 BOOT 按键所在脚，因此这里只做输出（按键 demo 见 1.3）。
 *
 * 观察点：RX 灯慢闪（1Hz），TX 灯快闪（5Hz），相位错开便于区分。
 */

#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo_blink";

/* 节奏（毫秒） */
#define BLINK_RX_HALF_MS 500   /* 1 Hz */
#define BLINK_TX_HALF_MS 100   /* 5 Hz */

static void blink_task(void *arg)
{
    (void)arg;
    bool state = false;
    int tick = 0;

    while (1) {
        /* 每 100ms 一个节拍：TX 每拍翻转，RX 每 5 拍翻转 */
        state = !state;
        edk60_led_set(1, state);          /* TX 灯（IO9） */
        if ((tick % 5) == 0) {
            edk60_led_set(0, state);      /* RX 灯（IO8） */
        }
        tick++;

        if ((tick % 50) == 0) {
            ESP_LOGI(TAG, "blink 运行中（第 %d 拍）：RX=%ums 半周期, TX=%ums 半周期",
                     tick, BLINK_RX_HALF_MS, BLINK_TX_HALF_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(BLINK_TX_HALF_MS));
    }
}

void demo_blink_start(void)
{
    ESP_LOGI(TAG, "=== blink：GPIO 输出验证 ===");
    ESP_LOGI(TAG, "LED RX 丝印 -> IO%d，LED TX 丝印 -> IO%d（低电平点亮）",
             edk60_led_pin(0), edk60_led_pin(1));

    ESP_ERROR_CHECK(edk60_led_init());

    /* 起始状态：全灭，随后交给闪烁任务 */
    edk60_led_set_all(false);

    xTaskCreate(blink_task, "blink", 3072, NULL, 4, NULL);
}


