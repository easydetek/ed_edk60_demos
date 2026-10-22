/*
 * EDK60 Demo 1.4 —— uart_echo
 *
 * 目的：验证扩展排针上的硬件 UART 通路（发出去、收回来都对）。
 *
 * 硬件事实（原理图 H2 = PM254-1-08-Z-8.5）：
 *   H2 的 D0/RX = IO16，D1/TX = IO17，两脚各串 10R 隔离电阻。
 *   这两脚同时也是 ESP32-C6 的 UART0 默认脚 —— 而本工程的控制台就在 UART0 上
 *   （USB 侧只是第二输出口）。因此 UART0 会被占用，本 demo 使用 **UART1**。
 *
 * 接线（用 USB-TTL 串口板 / 逻辑分析仪）：
 *   USB-TTL TX  ->  H2 的 D0/RX (IO16)   ← 本 demo 的接收脚
 *   USB-TTL RX  <-  H2 的 D1/TX (IO17)   ← 本 demo 的发送脚
 *   USB-TTL GND ->  H2 的 GND
 *   注意：串口板必须是 3.3V 电平；5V 电平的发送端不要直连 IO16。
 *
 * 行为：
 *   - 收到什么回什么（原文回显），并按行给出 ASCII + HEX 双视图；
 *   - 每 5 秒主动打印一行带序号的「心跳」，用来验证 TX 方向；
 *   - 收到 CR/LF 时按换行处理，避免日志串行。
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo_uart";

#define DEMO_UART_PORT      UART_NUM_1
#define DEMO_UART_TX_PIN    EDK60_PIN_D1_TX   /* H2 D1/TX = IO17 */
#define DEMO_UART_RX_PIN    EDK60_PIN_D0_RX   /* H2 D0/RX = IO16 */
#define DEMO_UART_BAUD      115200
#define DEMO_UART_BUF_SIZE  2048

static void uart_echo_task(void *arg)
{
    (void)arg;

    uint8_t buf[256];
    char ascii[129];
    char hex[3 * 64 + 1];
    uint32_t rx_total = 0;
    uint32_t lines = 0;

    while (1) {
        int n = uart_read_bytes(DEMO_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (n <= 0) {
            continue;
        }

        rx_total += (uint32_t)n;
        lines++;

        /* 原样回显 */
        uart_write_bytes(DEMO_UART_PORT, (const char *)buf, n);
        uart_wait_tx_done(DEMO_UART_PORT, pdMS_TO_TICKS(100));

        /* ASCII 视图（不可打印字符用 . 代替） */
        int limit = n > 128 ? 128 : n;
        for (int i = 0; i < limit; i++) {
            ascii[i] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.';
        }
        ascii[limit] = '\0';

        /* HEX 视图（最多 64 字节，避免刷屏） */
        int hexn = n > 64 ? 64 : n;
        hex[0] = '\0';
        for (int i = 0; i < hexn; i++) {
            char tmp[4];
            snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
            strncat(hex, tmp, sizeof(hex) - strlen(hex) - 1);
        }

        printf("[UART1 收] %d 字节（累计 %" PRIu32 " 字节 / %" PRIu32 " 批）\n",
               n, rx_total, lines);
        printf("  ASCII: %s%s\n", ascii, n > limit ? " ..." : "");
        printf("  HEX  : %s%s\n", hex, n > hexn ? "..." : "");
    }
}

static void uart_tx_heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;

    while (1) {
        char line[128];
        int len = snprintf(line, sizeof(line),
                           "[EDK60 UART1 心跳 #%" PRIu32 "] TX=IO%d RX=IO%d %d-8-N-1\r\n",
                           ++seq, DEMO_UART_TX_PIN, DEMO_UART_RX_PIN, DEMO_UART_BAUD);
        uart_write_bytes(DEMO_UART_PORT, line, len);
        ESP_LOGI(TAG, "已通过 UART1 发送心跳 #%" PRIu32, seq);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void demo_uart_echo_start(void)
{
    ESP_LOGI(TAG, "=== uart_echo：硬件 UART1 收发验证 ===");
    ESP_LOGI(TAG, "控制台占用 UART0(IO%d/IO%d)，本 demo 使用 UART1(TX=IO%d, RX=IO%d)，%d bps",
             EDK60_PIN_D0_RX, EDK60_PIN_D1_TX, DEMO_UART_TX_PIN, DEMO_UART_RX_PIN, DEMO_UART_BAUD);
    ESP_LOGI(TAG, "接线：外部串口板 TX->IO%d，RX<-IO%d，GND 共地（3.3V 电平）",
             DEMO_UART_RX_PIN, DEMO_UART_TX_PIN);

    const uart_config_t cfg = {
        .baud_rate = DEMO_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(DEMO_UART_PORT, DEMO_UART_BUF_SIZE, DEMO_UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DEMO_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DEMO_UART_PORT, DEMO_UART_TX_PIN, DEMO_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_rx_timeout(DEMO_UART_PORT, 5));

    ESP_LOGI(TAG, "UART1 就绪，开始回显。请从 IO%d 发数据过来。", DEMO_UART_RX_PIN);

    xTaskCreate(uart_echo_task, "uart_rx", 4096, NULL, 5, NULL);
    xTaskCreate(uart_tx_heartbeat_task, "uart_tx", 3072, NULL, 3, NULL);
}


