/*
 * EDK60 板级支持包（Board Support Package）
 *
 * 板卡：EDK60（EDK60_VA V1.0，2026-06-15，嘉立创EDA 原理图 docs/SCH_EDK60_VA.pdf）
 * 主控：ESP32-C6-MINI-1-N4（4MB Flash，无 PSRAM，53 脚模组）
 *
 * 说明
 * ----
 * 1. 本文件中的 GPIO 编号全部来自原理图网络标号，不是「按经验猜」的默认值。
 *    改板后必须重新核对原理图并同步本文件。
 * 2. 板上固定资源（LED / 按键 / 雷达接口）与扩展排针（H1/H2/H3/H4/H5/H6）
 *    的对应关系见 docs/EDK60-pinmap.md。
 * 3. 多个 demo 共用本组件，因此这里只提供「板上真实存在的资源」，
 *    不提供任何 demo 专用逻辑。
 * 4. 删除操作守则：本组件不含任何删除/擦写行为，nvs 擦除等破坏性操作只在
 *    对应 demo 中由用户显式触发。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 板卡与芯片基本信息
 * =========================================================================== */

#define EDK60_BOARD_NAME        "EDK60"
#define EDK60_BOARD_REV         "EDK60_VA V1.0 (2026-06-15)"
#define EDK60_MODULE_NAME       "ESP32-C6-MINI-1-N4"
#define EDK60_FLASH_SIZE_MB     (4)
#define EDK60_FEATURE_RADAR_LDO (1)   /* 板载独立 3.3V_RADAR LDO（U4），由 IO15 使能 */

/* ===========================================================================
 * 板上固定资源（原理图网络标号 -> GPIO）
 * ===========================================================================
 *
 *  LED5 "RADAR"  红色  3.3V_RADAR 经 330R 常亮          -> 不是 GPIO
 *                       （注意：它由 3.3V_RADAR 供电，因此**跟随雷达电源通断**；
 *                         与 LED4 的区别是它挂在 LDO 输出端，反映的是"电源轨"而非"控制脚"）
 *  LED4 "L"      橙色  **IO15** -> 330R -> LED -> GND    高电平点亮  ← 由 IO15 驱动！
 *  LED3 "TX"     橙色  IO9  -> 330R -> LED -> GND        低电平点亮
 *  LED2 "RX"     橙色  IO8  -> 330R -> LED -> GND        低电平点亮
 *  LED1 "ON"     绿色  3.3V 经 330R 常亮                 -> 不是 GPIO，电源指示
 *
 *  ⚠ 因此板上可被程序点亮的 LED 实际有 **3 颗**（IO8 / IO9 / IO15），
 *    但 IO15 那颗兼具「雷达电源使能」功能，见下文雷达段落。
 */

/** LED2 / 丝印 "RX"：IO8 -> 330R -> LED -> GND（低电平点亮） */
#define EDK60_PIN_LED_RX        GPIO_NUM_8
/** LED3 / 丝印 "TX"：IO9 -> 330R -> LED -> GND（低电平点亮） */
#define EDK60_PIN_LED_TX        GPIO_NUM_9
#define EDK60_LED_ACTIVE_LEVEL  (0)     /* 0 = 低电平点亮 */

/** 可被 GPIO 点亮的编号（含逻辑电平语义） */
#define EDK60_LED_GPIO_COUNT    (2)

/**
 * BOOT 按键 SW2：IO9 经 TS-1088-AR020016 接 GND，模组内/外部 4.7k 上拉。
 * 注意：IO9 是 C6 的 BOOT strapping 脚，复位瞬间为低会进入下载模式 ——
 *       上电时机不能按住，复位后按键功能才可用。
 */
#define EDK60_PIN_BUTTON_BOOT   EDK60_PIN_LED_TX   /* 与 LED_TX 同一个 IO9 */
#define EDK60_BUTTON_ACTIVE_LEVEL (0)              /* 0 = 按下为低 */

/** RESET 按键 SW1：直接拉 EN 脚，非 GPIO，软件不可读 */
#define EDK60_HAS_RESET_BUTTON  (1)

/* ===========================================================================
 * 雷达（mmWave）接口 H4
 * ===========================================================================
 *  H4 为 8 pin 排针 PM200-1-08-Z-3.0-C，用于插拔雷达模组。
 *  丝印：  RES  P  V  G  T  R  A
 *  功能：  RES=复位  P=3.3V_RADAR  V=3.3V_RADAR  G=GND
 *          T=IO14（雷达 TX -> MCU RX）
 *          R=IO15（RADAR_PWR_EN：IO15 驱动 U4 的 EN，并顺带点亮 LED4）
 *          A=IO2/IO3/IO4/IO5（4 路 ADC 输入，用于模拟量雷达模组）
 *
 *  !! 关键硬件事实（2026-10-14 逐层读图核实，勿凭印象改写）!!
 *
 *  链路 1（电源使能）：
 *      U4(AP7362-33SP-13).EN ── R2(10k) ── GND      ← 10k 下拉，不是上拉！
 *                          └── IO15（网络名 RADAR_PWR_EN）
 *
 *  链路 2（状态指示）：
 *      IO15 ── R9(330Ω) ── LED4(KT-0603Y, 橙色, 丝印 "L") ── GND   （高电平点亮）
 *
 *  由此得出四条必须遵守的结论：
 *    1. **上电默认关闭**：R2 是 10k 下拉到 GND，复位后 EN=0，3.3V_RADAR 默认不输出。
 *       这对插拔式雷达模组是安全默认值（可热插拔，不必先断电）。
 *    2. **使能 = IO15 输出高电平**（推挽输出即可，10k 下拉只需 0.33mA）。
 *    3. **关闭 = IO15 输出低电平**（或配成高阻让下拉把 EN 拉低）。
 *    4. 丝印 "L" 的橙色 LED4 是「雷达电源指示」：**亮 = 3.3V_RADAR 已使能**，
 *       由 IO15 直接驱动（3.3V / 330Ω ≈ 4mA）。也就是说这颗灯的明灭可用来
 *       直观校验 IO15 与雷达电源状态，无需万用表。
 *
 *  推论（低功耗相关）：把 IO15 拉低不仅关闭雷达 LDO，LED4 也会同步熄灭，
 *  不会产生额外的静态电流。这是把指示与使能共用一根脚带来的好处。
 *
 *  ⚠ 已知的读图不确定性（**必须上板实测确认，见 docs/EDK60-pinmap.md H-01/H-02**）
 *  ------------------------------------------------------------------------------
 *  在 U4 的 EN 走线区域，原理图上**同时**出现 `RADAR_PWR_EN` 与 `IO14` 两个网络标号，
 *  单页原理图无法唯一判定 EN 到底接 IO15 还是 IO14：
 *
 *    可能 A（本 BSP 采用的工作假设）：
 *        EN 接 **IO15**（RADAR_PWR_EN 即 IO15 的网络名），IO14 = 雷达 UART RX；
 *    可能 B：
 *        EN 接 **IO14**（IO14 是 EN 的网络名，RADAR_PWR_EN 只是功能注释），
 *        此时 IO15 仅驱动 LED4，且**雷达将没有可用的 UART RX**。
 *
 *  两种可能下，IO15 都驱动 LED4（这一点由 R9(330Ω)→LED4 的走线独立确认），
 *  因此「LED4 明灭」只能证明 IO15 在驱动指示，**不能单独证明电源使能脚是谁**。
 *  判定方法（一次上板即可定论）：
 *      用万用表量 H4 的 P/V 脚（3.3V_RADAR）与 IO14 电平：
 *        · 量到 3.3V 且 IO14 无变化  → 可能 A 成立，本 BSP 无需改动；
 *        · 量到 0V 且 IO14 上有高电平变化 → 可能 B 成立，需要把
 *          `EDK60_PIN_RADAR_PWR_EN` 指向 IO14 并重新评估雷达 UART 方案。
 *  radar_power / usb_serial_bridge 两个 demo 已内置该自检的日志提示。
 */

/* ===== 雷达引脚定论（2026-10-15 上板实测，已经实测确认，勿再凭图纸推断）=====
 * 实测方法与证据：
 *   ① IO15 拉高 → 丝印 "L" 橙灯亮，但 3.3V_RADAR 不起（丝印 "RADAR" 红灯灭）
 *      ⇒ IO15 **只驱动指示灯**，不控制电源；
 *   ② IO14 上下拉高/拉低 → 红灯跟着亮/灭（实测闪烁 5 次均同步）
 *      ⇒ **IO14 = U4 的 EN（RADAR_PWR_EN）**，拉高才上电；
 *   ③ 模组插在 **H3** 时，在 **IO4**（H3 的 T 脚）上收到正常数据帧（see below）。
 *
 * 因此：
 *   · 电源使能 = IO14；
 *   · 指示灯（橙，丝印 "L"）= IO15，与电源无关；
 *   · 雷达数据线取决于插哪个排针：
 *       H3（A=IO0 / R=IO1 / T=IO4）→ 实测数据在 **IO4**；
 *       H4（T=IO14 / R=RADAR_PWR_EN / A=IO2..IO5）→ T 与 EN 同网，与电源使能冲突。
 *
 * 结论：本板要同时得到「可控电源 + 可读数据」，模组应插 **H3**，
 *       并用 IO14 作电源使能、IO4 作 UART RX。 */
#define EDK60_PIN_RADAR_UART_RX GPIO_NUM_4   /* 实测：模组 TX -> MCU（H3 的 T 脚） */
#define EDK60_PIN_RADAR_UART_TX GPIO_NUM_5   /* 实测：MCU -> 模组 RX（H3 的 A 脚）；发命令走这根 */
#define EDK60_PIN_RADAR_PWR_EN  GPIO_NUM_14  /* 实测：U4 的 EN = IO14（拉高上电） */
#define EDK60_RADAR_PWR_EN_ACTIVE_LEVEL (1)  /* 1 = 输出高电平则使能 3.3V_RADAR */

/** 雷达电源状态指示 LED4（丝印 "L"，橙色）所在 GPIO —— 与使能脚共用 IO15 */
#define EDK60_PIN_RADAR_LED     GPIO_NUM_15  /* 实测：丝印 "L" 橙灯由 IO15 直接驱动，与电源 EN 无关 */
#define EDK60_RADAR_LED_ACTIVE_LEVEL (1)     /* 1 = 高电平点亮 */

/** H4-A：4 路模拟输入（同时是 H3 的 A2..A5、H6 的 A0..A3） */
#define EDK60_PIN_RADAR_ADC0    GPIO_NUM_2
#define EDK60_PIN_RADAR_ADC1    GPIO_NUM_3
#define EDK60_PIN_RADAR_ADC2    GPIO_NUM_4
#define EDK60_PIN_RADAR_ADC3    GPIO_NUM_5

/* ===========================================================================
 * 扩展排针（UNO 形式）
 * ===========================================================================
 *
 *  H1  PM254-1-10-Z-8.5（数字/电源）：IOREF RESET 3V3 5V GND GND VIN SCL SDA AREF
 *      其中 SDA=IO6, SCL=IO7 与 H2 的 D14/D15 位置同网络。
 *  H2  PM254-1-08-Z-8.5（数字）：D0/RX D1/TX D2 D3 D4 D5 D6 D7
 *  H3  PM200-1-08-Z-3.0-C（模拟/电源）：RES A R T G V P
 *  H4  PM200-1-08-Z-3.0-C（雷达接口）：RES P V G T R A
 *  H5  PM254-1-08-Z-8.5（电源）：NC IOREF RESET 3V3 5V GND GND VIN
 *  H6  PM254-1-06-Z-8.5（模拟）：A1 A2 A3 A4 A5
 *
 *  所有数字/模拟 IO 均经 10R（R21..R47）隔离电阻引出。
 */

/* --- H2 数字口（Arduino D0..D7） --- */
#define EDK60_PIN_D0_RX         GPIO_NUM_16  /* 也是 UART0 默认 RX（控制台） */
#define EDK60_PIN_D1_TX         GPIO_NUM_17  /* 也是 UART0 默认 TX（控制台） */
#define EDK60_PIN_D2            GPIO_NUM_18
#define EDK60_PIN_D3            GPIO_NUM_19
#define EDK60_PIN_D4            GPIO_NUM_20
#define EDK60_PIN_D5            GPIO_NUM_21
#define EDK60_PIN_D6            GPIO_NUM_22
#define EDK60_PIN_D7            GPIO_NUM_23

/* --- H1 的 D8..D13（经 10R） --- */
#define EDK60_PIN_D8            GPIO_NUM_8   /* 同 LED_RX */
#define EDK60_PIN_D9            GPIO_NUM_9   /* 同 LED_TX / BOOT */
#define EDK60_PIN_D10           GPIO_NUM_2
#define EDK60_PIN_D11           GPIO_NUM_3
#define EDK60_PIN_D12           GPIO_NUM_4
#define EDK60_PIN_D13           GPIO_NUM_5

/* --- I2C（H1 丝印 SDA/SCL；C6 的 LP_I2C 默认脚） --- */
#define EDK60_PIN_I2C_SDA       GPIO_NUM_6
#define EDK60_PIN_I2C_SCL       GPIO_NUM_7
#define EDK60_I2C_DEFAULT_FREQ_HZ (100000)
#define EDK60_PIN_I2C_SDA_ADC_CH  ADC_CHANNEL_6   /* IO6 同时是 ADC1_CH6 */
#define EDK60_PIN_I2C_SCL_ADC_CH  (-1)            /* IO7 无 ADC 功能 */

/* --- H3/H6 模拟口（A0..A5，全部落在 ADC1） --- */
#define EDK60_PIN_A0            GPIO_NUM_0
#define EDK60_PIN_A1            GPIO_NUM_1
#define EDK60_PIN_A2            GPIO_NUM_2
#define EDK60_PIN_A3            GPIO_NUM_3
#define EDK60_PIN_A4            GPIO_NUM_4
#define EDK60_PIN_A5            GPIO_NUM_5

#define EDK60_ADC_UNIT          ADC_UNIT_1
#define EDK60_ADC_CH_A0         ADC_CHANNEL_0
#define EDK60_ADC_CH_A1         ADC_CHANNEL_1
#define EDK60_ADC_CH_A2         ADC_CHANNEL_2
#define EDK60_ADC_CH_A3         ADC_CHANNEL_3
#define EDK60_ADC_CH_A4         ADC_CHANNEL_4
#define EDK60_ADC_CH_A5         ADC_CHANNEL_5
#define EDK60_ADC_CH_SDA        ADC_CHANNEL_6   /* IO6，H1 的 SDA */
#define EDK60_ADC_CHANNEL_COUNT (6)             /* A0..A5 */

/* ===========================================================================
 * 被外设/系统占用的 GPIO（demo 选引脚时的避让清单）
 * =========================================================================== */

/** USB Serial/JTAG 固定占用的两脚（EDK60 上接到 USB-C），不可他用 */
#define EDK60_PIN_USB_DM        GPIO_NUM_12
#define EDK60_PIN_USB_DP        GPIO_NUM_13

/** C6 strapping 脚：上电复位时的电平决定启动模式，外部电路不能强驱动 */
#define EDK60_PIN_STRAP_4       GPIO_NUM_4
#define EDK60_PIN_STRAP_5       GPIO_NUM_5
#define EDK60_PIN_STRAP_8       GPIO_NUM_8   /* LED_RX 挂在这里 */
#define EDK60_PIN_STRAP_9       GPIO_NUM_9   /* LED_TX / BOOT 挂在这里 */
#define EDK60_PIN_STRAP_15      GPIO_NUM_15  /* RADAR_PWR_EN 挂在这里 */

/** C6 的 RTC/EXT1 唤醒只支持 GPIO0..7（深睡唤醒 demo 依赖此约束） */
#define EDK60_EXT1_WAKEUP_GPIO_MIN  (0)
#define EDK60_EXT1_WAKEUP_GPIO_MAX  (7)

/* ===========================================================================
 * 通用助手
 * =========================================================================== */

/** 板卡 + 芯片 + SDK 信息横幅（写 stdout，无返回值） */
void edk60_print_banner(const char *demo_name);

/** 打印引脚映射表（受 CONFIG_APP_EDK60_PRINT_PINMAP 控制） */
void edk60_print_pinmap(void);

/** 打印复位原因 / 深睡唤醒原因 */
void edk60_print_reset_reason(void);

/* --- LED（低电平点亮） --- */

/** 初始化两路 GPIO LED（IO8/IO9），初始为灭 */
esp_err_t edk60_led_init(void);

/** 设置 LED 亮灭。led_index 0=LED_RX(IO8), 1=LED_TX(IO9) */
esp_err_t edk60_led_set(int led_index, bool on);

/** 翻转 LED */
esp_err_t edk60_led_toggle(int led_index);

/** 点亮全部（指示灯自检用，等待若干毫秒后自行关闭由调用方决定） */
esp_err_t edk60_led_set_all(bool on);

/** 取得 LED 的 GPIO 编号，越界返回 GPIO_NUM_NC */
gpio_num_t edk60_led_pin(int led_index);

/* --- 按键（BOOT，IO9，低有效） --- */

typedef enum {
    EDK60_BUTTON_EVENT_PRESSED = 0,  /* 按下（下降沿，已消抖） */
    EDK60_BUTTON_EVENT_RELEASED,     /* 松开（上升沿，已消抖） */
} edk60_button_event_t;

typedef void (*edk60_button_cb_t)(edk60_button_event_t event, uint32_t hold_ms, void *user_ctx);

/** 以轮询方式初始化按键（不需要中断） */
esp_err_t edk60_button_init_polling(void);

/** 读取当前按键是否按下（已做 20ms 时间窗消抖） */
bool edk60_button_is_pressed(void);

/**
 * 以中断方式初始化按键并注册回调（回调在 esp_timer 任务上下文执行，可调用 printf）。
 * 使用 GPIO ISR service + 队列 + esp_timer 消抖，避免在 ISR 里做耗时操作。
 */
esp_err_t edk60_button_init_isr(edk60_button_cb_t cb, void *user_ctx);

/* --- I2C 主机总线（SDA=IO6, SCL=IO7） --- */

typedef void *edk60_i2c_bus_handle_t;

/** 创建 I2C 主机总线（内部上拉，100kHz）。多次调用返回同一总线。 */
esp_err_t edk60_i2c_init(edk60_i2c_bus_handle_t *out_bus);

/** 探测 7bit 地址是否存在器件（单次事务，无副作用） */
esp_err_t edk60_i2c_probe(edk60_i2c_bus_handle_t bus, uint8_t addr_7bit, bool *out_present);

/** 释放总线 */
esp_err_t edk60_i2c_deinit(edk60_i2c_bus_handle_t bus);

/* --- 雷达电源（IO15 -> U4.EN，10k 下拉到 GND；高 = 使能） --- */

/**
 * 初始化雷达电源控制脚（推挽输出），并置为**关闭**状态。
 *
 * 硬件：U4(AP7362-33SP-13).EN 经 R2(10k) 下拉到 GND，且 EN 接 IO15。
 *       因此复位后 EN=0 ⇒ 3.3V_RADAR 默认不输出（插拔雷达模组是安全的）。
 *       IO15 同时经 R9(330Ω) 驱动 LED4（橙色，丝印 "L"）作为电源指示。
 */
esp_err_t edk60_radar_power_init(void);

/** 打开 3.3V_RADAR（IO15 输出高 → EN=1；LED4 同步点亮） */
esp_err_t edk60_radar_power_on(void);

/** 关闭 3.3V_RADAR（IO15 输出低 → EN=0；LED4 同步熄灭） */
esp_err_t edk60_radar_power_off(void);

/** 读取当前 EN 脚电平（true = 雷达电源已使能） */
bool edk60_radar_power_is_on(void);

/* --- 控制台 --- */

/**
 * 建立控制台输入能力（仅当控制台是本芯片的原生 USB Serial/JTAG 时有效）。
 * 返回 ESP_ERR_NOT_SUPPORTED 表示当前主控制台不是 USB Serial/JTAG，
 * 此时 stdin 由 UART 驱动提供，本函数无需做任何事。
 */
esp_err_t edk60_console_install_usb_serial_jtag(void);

#ifdef __cplusplus
}
#endif
