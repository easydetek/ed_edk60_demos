/*
 * EDK60 Demo 1.1 —— hello_board
 *
 * 目的：验证「板子活着、串口通、两个 GPIO LED 可控」。
 * 这是拿到新板子后的第一个 demo：
 *   - 串口打印板卡信息 / 复位原因 / 引脚映射（由 app_main 的横幅负责）；
 *   - 依次点亮 LED2(RX/IO8) 与 LED3(TX/IO9) 做指示灯自检；
 *   - 之后每 5 秒打印一次运行时长与空闲内存，作为「心跳」。
 *
 * 观察点：
 *   1) 打开串口（115200-8-N-1），复位后应看到 EDK60 横幅；
 *   2) 上电瞬间两颗黄灯（丝印 RX / TX）各闪一下；
 *   3) 丝印 ON 的绿灯与 L 的橙灯常亮（直接由 3.3V 供电，不受程序控制）；
 *   4) 若插了雷达模组，丝印 RADAR 的红灯也会亮（3.3V_RADAR 供电）。
 */

#include <stdio.h>

#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo_hello";

static void heartbeat_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        int64_t up_s = (esp_timer_get_time() - t0) / 1000000;
        printf("[心跳] 运行 %lld 秒 | 空闲堆 %u 字节 | 最小空闲堆 %u 字节 | 复位原因 %d\n",
               (long long)up_s,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
               (int)esp_reset_reason());
    }
}

void demo_hello_board_start(void)
{
    ESP_LOGI(TAG, "=== hello_board：板卡自检与心跳 ===");

    /* --- 1. LED 自检 --- */
    ESP_ERROR_CHECK(edk60_led_init());
    ESP_LOGI(TAG, "LED 自检：依次点亮 RX(IO%d) / TX(IO%d)",
             edk60_led_pin(0), edk60_led_pin(1));

    for (int i = 0; i < EDK60_LED_GPIO_COUNT; i++) {
        edk60_led_set(i, true);
        vTaskDelay(pdMS_TO_TICKS(250));
        edk60_led_set(i, false);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    for (int i = 0; i < EDK60_LED_GPIO_COUNT; i++) {
        edk60_led_set(i, true);
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    edk60_led_set_all(false);

    /* --- 2. 雷达电源脚只初始化，不改动状态（保持断电/插入安全） --- */
    ESP_ERROR_CHECK(edk60_radar_power_init());
    ESP_LOGI(TAG, "雷达电源 EN(IO%d) 当前电平 = %d（1=3.3V_RADAR 已使能）",
             EDK60_PIN_RADAR_PWR_EN, (int)edk60_radar_power_is_on());

    /* --- 3. 空闲外设提示 --- */
    ESP_LOGI(TAG, "其他可用资源：I2C(SDA=IO6/SCL=IO7)、ADC1(A0..A5=IO0..IO5)、"
                  "UART1(D0/RX=IO16, D1/TX=IO17)、按键 BOOT(IO9)");

    xTaskCreate(heartbeat_task, "hb", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "hello_board 就绪：请观察串口心跳与 LED 表现");
}


