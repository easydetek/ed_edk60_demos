/*
 * EDK60 多工程 Demo —— 应用入口
 *
 * menuconfig -> "EDK60 Demo" -> "选择要编译的 Demo" 决定了实际运行哪一个 demo。
 * 分派逻辑见 components/edk60_app/src/demo_dispatch.c。
 */

#include "edk60_demo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "edk60_main";

void app_main(void)
{
    edk60_run_selected_demo();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGD(TAG, "app_main 心跳（当前 demo: %s）", edk60_current_demo_name());
    }
}
