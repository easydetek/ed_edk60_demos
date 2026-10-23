/*
 * EDK60 应用组件 —— Demo 分派器
 *
 * 这里把 menuconfig 的选择（CONFIG_APP_EDK60_DEMO_*）映射为唯一的 demo 入口。
 * 所有 demo 都提供 void demo_get_started(void)（见 components/edk60_app/include/edk60_demo.h），
 * 因此本文件只需选一个调用。
 *
 * 新增 demo：在 main/Kconfig.projbuild 加 config，在组件 CMakeLists 加源文件，
 * 然后在本文件补一个 #elif 分支并在 edk60_demo.h 加声明。
 */

#include <string.h>

#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "edk60_app";

/* 5.2 radar_bridge_edv11p 的入口。
 * 按惯例声明应放在 components/edk60_app/include/edk60_demo.h，但本次任务的
 * 改动范围被限定在「新增 demo 自身 + 分派器」，为不改动被 17 个 demo 共用的
 * 头文件，这里直接做前置声明（效果等价，且把影响面收在一个文件里）。 */
void demo_radar_bridge_edv11p_start(void);

/* 5.3 radar_bridge_edv163 的入口。理由同上（不动被 18 个 demo 共用的头文件），
 * 同样在这里做前置声明。 */
void demo_radar_bridge_edv163_start(void);

/* 5.4 radar_bridge_edv163_gesture 的入口。理由同上（不动被 19 个 demo 共用的
 * 头文件），同样在这里做前置声明。 */
void demo_radar_bridge_edv163_gesture_start(void);

/* --------------------------------------------------------------------------
 * demo 选择：与 main/Kconfig.projbuild 的 choice APP_EDK60_DEMO 一一对应
 * -------------------------------------------------------------------------- */
#if defined(CONFIG_APP_EDK60_DEMO_HELLO_BOARD)
#define EDK60_DEMO_NAME       "hello_board"
#define EDK60_DEMO_CALL()     demo_hello_board_start()
#elif defined(CONFIG_APP_EDK60_DEMO_BLINK)
#define EDK60_DEMO_NAME       "blink"
#define EDK60_DEMO_CALL()     demo_blink_start()
#elif defined(CONFIG_APP_EDK60_DEMO_BUTTON)
#define EDK60_DEMO_NAME       "button"
#define EDK60_DEMO_CALL()     demo_button_start()
#elif defined(CONFIG_APP_EDK60_DEMO_UART_ECHO)
#define EDK60_DEMO_NAME       "uart_echo"
#define EDK60_DEMO_CALL()     demo_uart_echo_start()
#elif defined(CONFIG_APP_EDK60_DEMO_ADC_READ)
#define EDK60_DEMO_NAME       "adc_read"
#define EDK60_DEMO_CALL()     demo_adc_read_start()
#elif defined(CONFIG_APP_EDK60_DEMO_WIFI_STA)
#define EDK60_DEMO_NAME       "wifi_sta"
#define EDK60_DEMO_CALL()     demo_wifi_sta_start()
#elif defined(CONFIG_APP_EDK60_DEMO_WIFI_SOFTAP)
#define EDK60_DEMO_NAME       "wifi_softap"
#define EDK60_DEMO_CALL()     demo_wifi_softap_start()
#elif defined(CONFIG_APP_EDK60_DEMO_BLE_UART)
#define EDK60_DEMO_NAME       "ble_uart"
#define EDK60_DEMO_CALL()     demo_ble_uart_start()
#elif defined(CONFIG_APP_EDK60_DEMO_BLE_SCAN)
#define EDK60_DEMO_NAME       "ble_scan"
#define EDK60_DEMO_CALL()     demo_ble_scan_start()
#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_POWER)
#define EDK60_DEMO_NAME       "radar_power"
#define EDK60_DEMO_CALL()     demo_radar_power_start()
#elif defined(CONFIG_APP_EDK60_DEMO_LED_SEQUENCE)
#define EDK60_DEMO_NAME       "led_sequence"
#define EDK60_DEMO_CALL()     demo_led_sequence_start()
#elif defined(CONFIG_APP_EDK60_DEMO_I2C_SCAN)
#define EDK60_DEMO_NAME       "i2c_scan"
#define EDK60_DEMO_CALL()     demo_i2c_scan_start()
#elif defined(CONFIG_APP_EDK60_DEMO_DEEP_SLEEP)
#define EDK60_DEMO_NAME       "deep_sleep"
#define EDK60_DEMO_CALL()     demo_deep_sleep_start()
#elif defined(CONFIG_APP_EDK60_DEMO_NVS_RW)
#define EDK60_DEMO_NAME       "nvs_rw"
#define EDK60_DEMO_CALL()     demo_nvs_rw_start()
#elif defined(CONFIG_APP_EDK60_DEMO_USB_SERIAL_BRIDGE)
#define EDK60_DEMO_NAME       "usb_serial_bridge"
#define EDK60_DEMO_CALL()     demo_usb_serial_bridge_start()
#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE)
#define EDK60_DEMO_NAME       "radar_bridge"
#define EDK60_DEMO_CALL()     demo_radar_bridge_start()
#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P)
#define EDK60_DEMO_NAME       "radar_bridge_edv11p"
#define EDK60_DEMO_CALL()     demo_radar_bridge_edv11p_start()
#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163)
#define EDK60_DEMO_NAME       "radar_bridge_edv163"
#define EDK60_DEMO_CALL()     demo_radar_bridge_edv163_start()
#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE)
#define EDK60_DEMO_NAME       "radar_bridge_edv163_gesture"
#define EDK60_DEMO_CALL()     demo_radar_bridge_edv163_gesture_start()
#else
#error "未选择任何 EDK60 Demo：请执行 idf.py menuconfig -> EDK60 Demo -> 选择要编译的 Demo"
#endif

const char *edk60_current_demo_name(void)
{
    return EDK60_DEMO_NAME;
}

void edk60_run_selected_demo(void)
{
/* 5.2 radar_bridge_edv11p / 5.3 radar_bridge_edv163 / 5.4 radar_bridge_edv163_gesture
 * 的「洁净门禁」：
 * 横幅 / 复位原因 / 引脚表**全部是 printf**，而本工程把 USB Serial/JTAG 配成了
 * 控制台的第二输出口 —— 这些字符会被复制进 USB 数据流。更糟的是横幅首行的板名
 * 以两个 ASCII 字节 "E""D" 开头（十六进制 0x45 0x44），正好等于这几款雷达的
 * 帧头（EDV11P 与 EDV163 的帧头都是 0x45 0x44；5.4 挥手固件的二进制点云帧
 * 帧头同样是 0x45 0x44，而它的主体是**没有校验的文本行**，被污染后只会表现为
 * "坏行"、"挥手方向偶尔不显示"，连校验失败这种明确信号都没有）。
 * 所以这三个 demo 下整块跳过，不依赖 sdkconfig 里那两项配置是否记得关（双保险）。 */
#if !defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P) && \
    !defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163) && \
    !defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV163_GESTURE)
#if CONFIG_APP_EDK60_STARTUP_BANNER
    edk60_print_banner(EDK60_DEMO_NAME);
    edk60_print_reset_reason();
#if CONFIG_APP_EDK60_PRINT_PINMAP
    edk60_print_pinmap();
#endif
#endif
#endif /* !RADAR_BRIDGE_EDV11P && !RADAR_BRIDGE_EDV163 && !RADAR_BRIDGE_EDV163_GESTURE */

#if CONFIG_APP_EDK60_BOOT_DELAY_MS > 0
    ESP_LOGI(TAG, "启动等待 %d ms（便于 USB 主机完成枚举后再看日志）",
             CONFIG_APP_EDK60_BOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_EDK60_BOOT_DELAY_MS));
#endif

    ESP_LOGI(TAG, "启动 Demo: %s", EDK60_DEMO_NAME);

    EDK60_DEMO_CALL();

    ESP_LOGI(TAG, "Demo '%s' 初始化完成（主任务转为低频心跳，请查看各 demo 自己的任务日志）",
             EDK60_DEMO_NAME);
}
