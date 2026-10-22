/*
 * EDK60 Demo 统一接口
 *
 * 每个 demo 源文件实现一个 demo_<名字>_start()，由 components/edk60_app/src/demo_dispatch.c
 * 根据 menuconfig 的选择调用其中的一个。
 *
 * 约定
 * ----
 *   * 函数返回 void，不阻塞（自己创建任务后返回），或者自行进入循环（如回显类 demo）。
 *   * 失败用 ESP_ERROR_CHECK / ESP_LOGE 明确暴露，不要静默吞掉。
 *   * 所有 GPIO 编号取自 edk60_board.h，禁止在 demo 里硬编码引脚号。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* --- 1. 基础外设组 --- */
void demo_hello_board_start(void);       /* 1.1 板卡信息 + 心跳 */
void demo_blink_start(void);             /* 1.2 LED 闪灯 */
void demo_button_start(void);            /* 1.3 BOOT 按键 */
void demo_uart_echo_start(void);         /* 1.4 UART1 回显 */
void demo_adc_read_start(void);          /* 1.5 ADC1 六通道扫描 */

/* --- 2. 无线组 --- */
void demo_wifi_sta_start(void);          /* 2.1 Wi-Fi STA 扫描/连接 */
void demo_wifi_softap_start(void);       /* 2.2 Wi-Fi SoftAP + DHCP */
void demo_ble_uart_start(void);          /* 2.3 BLE 串口透传 */
void demo_ble_scan_start(void);          /* 2.4 BLE 广播扫描 */

/* --- 3. 板载互动组 --- */
void demo_radar_power_start(void);       /* 3.1 3.3V_RADAR 使能时序 + 雷达 UART */
void demo_led_sequence_start(void);      /* 3.2 跑马灯 / 呼吸灯 */
void demo_i2c_scan_start(void);          /* 3.3 I2C 总线扫描 */

/* --- 4. 低功耗与存储组 --- */
void demo_deep_sleep_start(void);        /* 4.1 深睡与唤醒 */
void demo_nvs_rw_start(void);            /* 4.2 NVS 读写 */
void demo_usb_serial_bridge_start(void); /* 4.3 USB 原生串口 ⇄ 雷达 UART 桥 */

/* --- 5. 雷达透传组 --- */
void demo_radar_bridge_start(void);      /* 5.1 雷达 ⇄ UART ⇄ USB串口 ⇄ PC 纯字节透传 */

/** 当前 menuconfig 选中的 demo 名（由 demo_dispatch.c 提供） */
const char *edk60_current_demo_name(void);

/** 由 main 调用：打印横幅并启动选中的 demo */
void edk60_run_selected_demo(void);

#ifdef __cplusplus
}
#endif
