/*
 * EDK60 Demo 4.1 —— deep_sleep（深睡 + 定时 / EXT1 排针 / BOOT 按键 三种唤醒）
 *
 * 目的
 * ----
 *   1) 演示 ESP32-C6 的 deep sleep（深睡）以及三类唤醒源的配置、失败降级与判定；
 *   2) 演示用「RTC 保留内存（RTC_DATA_ATTR）」在深睡周期之间保存运行态；
 *   3) 给出「深睡期间板上还有谁在耗电」的量化提示与实测建议
 *      （关掉雷达电源后，固定负载只剩 LED1 与各稳压器静态电流）。
 *
 * 硬件依据（逐条可回溯，不是凭记忆写的）
 * --------------------------------------
 *   * 芯片：ESP32-C6-MINI-1-N4（4MB Flash，无 PSRAM）。
 *   * EXT1 / RTC GPIO 范围：esp_sleep.h 里 esp_sleep_enable_ext1_wakeup_io() 的
 *     io_mask 参数说明写明「ESP32-C6: 0-7」——即 C6 上只有 GPIO0..GPIO7 具备 RTC
 *     功能，可作 EXT1 唤醒源。本板把 IO0..IO5 引到排针 A0..A5
 *     （宏 EDK60_PIN_A0..EDK60_PIN_A5，见 components/edk60_board/include/edk60_board.h），
 *     所以「排针接地唤醒」只能用 A0..A5 其中之一（默认 A0=IO0）。
 *   * C6 没有 EXT0：soc/esp32c6/soc_caps.h 中没有定义 SOC_PM_SUPPORT_EXT0_WAKEUP，
 *     所以本文件**不使用** esp_sleep_enable_ext0_wakeup()。
 *   * BOOT 按键 = IO9（= LED3/TX，同一个脚）。IO9 **不在 0..7 之内**（它是 strapping
 *     脚，属于数字 pad / VDD3P3_CPU 域），因此**不能**用 EXT1 唤醒。
 *     可用的替代 API 是 esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown()
 *     （条件编译在 SOC_GPIO_SUPPORT_HP_PERIPH_PD_SLEEP_WAKEUP 之下，C6 该宏为 1）。
 *     但该 API 的文档同样要求掩码里的脚是「RTC function pads（VDD3P3_RTC 供电的
 *     pad）」——IO9 不满足，因此**大概率返回 ESP_ERR_INVALID_ARG**。
 *     本 demo 因此把 BUTTON 模式设计成「允许失败」：拿不到按键唤醒就自动降级为
 *     定时器唤醒兜底 + 打印降级提示，绝不 assert、也不会让板子睡死过去
 *     （睡死后只有 RESET 键能救回来，那是很糟的演示体验）。
 *   * EXT1 低电平唤醒的电气前提：该函数 @note 明确写「RTC 外设掉电时内部上下拉不
 *     起作用，需要外加电阻」。本板 A0..A5 只经 10R 隔离电阻引出到排针，**没有**外部
 *     上拉，因此实测时请给被唤醒脚外加 10k 上拉到 3.3V，否则悬空脚可能被噪声拉低而
 *     「莫名其妙立刻唤醒」。日志里会打印这条提醒。
 *   * 深睡会断开 USB：USB Serial/JTAG 不保活，唤醒后主机重新枚举串口，日志表现为
 *     「从复位重新开始」——这是正常现象，不是程序崩了。
 *   * 指示灯与深睡静态电流（依据 components/edk60_board/include/edk60_board.h，
 *     2026-10-14 更新后的板级结论，不要按旧版本记忆写）：
 *       - LED1 "ON"（绿，3.3V 经 330R）：**唯一真正常亮**的灯，不受程序控制，
 *         深睡期间照旧耗电 —— 它是深睡静态电流的固定组成部分。
 *       - LED4 "L"（橙）：**由 IO15 驱动**（高电平点亮），只是指示灯，
 *         不代表雷达有电；判断雷达是否上电请看丝印 "RADAR" 的红灯。
 *         深睡前调用 edk60_radar_power_off() 把它拉低，它就熄灭。
 *       - LED5 "RADAR"（红）：挂在 3.3V_RADAR 输出端，**跟随雷达电源通断**。
 *       所以「深睡前关掉雷达电源」一举三得：关 LDO、灭 LED4、灭 LED5。
 *       （LED 电流量级见 print_power_notes()，Vf 与实测值需上板确认。）
 *
 * 观察点
 * ------
 *   - 每次唤醒打印：唤醒原因（EXT1 触发脚翻译成排针丝印）、本次深睡时长、累计唤醒次数；
 *   - LED2(RX/IO8) 闪「唤醒源编码」次：1=定时唤醒 2=EXT1 排针 3=BOOT 按键；
 *   - LED3(TX/IO9) 闪「累计唤醒次数」次（上限 10 次，避免闪太久）；
 *   - 唤醒后有 3 秒交互窗口可按 BOOT：短按循环切换唤醒源模式，长按（>=1.5s）立即再深睡。
 *
 * !! IO9 两处复用冲突（改代码前必须知道）!!
 *   1) BOOT 按键与 LED3(TX) 是同一个 IO9。edk60_led_init() 把 IO9 配成输出，
 *      而 edk60_button_init_polling()/isr() 又把它改回输入（带上拉）。
 *      本 demo 的顺序固定为：先做 LED 指示（IO9=输出）→ 再初始化按键（IO9=输入）
 *      → 之后只用 IO8 做「等待按键」指示。不要打乱这个顺序。
 *   2) BUTTON 模式唤醒时，用户可能**仍按着** IO9（接地）。此时若把 IO9 配成输出并
 *      输出高电平，会经按键直接灌到 GND，可能损伤 pad。因此「唤醒瞬间按键已按下」
 *      时本 demo 跳过 LED3 指示，只用 LED2（见 led_indicator_prepare()）。
 *
 * !! IO9 是 strapping 脚 !!
 *   复位瞬间 IO9 为低会进入下载模式，所以「上电时按住 BOOT」不是 bug 而是设计行为。
 *   深睡唤醒同样要走一次复位流程、会重新采样 strapping 脚，因此「唤醒瞬间仍按住键」
 *   是否会被采样成下载模式，属于**需要上板实测确认**的项，本文件不做断言。
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "edk60_board.h"
#include "edk60_demo.h"

#include "esp_attr.h"        /* RTC_DATA_ATTR */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rtc_time.h"    /* esp_rtc_get_time_us：RTC 计数器，深睡期间继续走 */
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "demo_sleep";

/* ===========================================================================
 * 常量与配置
 * =========================================================================== */

/* 唤醒源模式：与 Kconfig APP_EDK60_SLEEP_MODE 的 0/1/2 一一对应 */
#define SLEEP_MODE_TIMER    0
#define SLEEP_MODE_EXT1     1
#define SLEEP_MODE_BUTTON   2
#define SLEEP_MODE_COUNT    3

/* 唤醒后的交互窗口与判键时长 */
#define WAKE_WINDOW_MS      3000   /* 留给用户按键的时间窗 */
#define LONG_PRESS_MS       1500   /* >= 此值判为「长按」→ 立刻再入深睡 */
#define POLL_INTERVAL_MS    20     /* 窗口内轮询周期（> 板级按键 20ms 消抖窗） */

/* LED 指示节奏 */
#define LED_RX_INDEX        0      /* LED2 / 丝印 RX / IO8 */
#define LED_TX_INDEX        1      /* LED3 / 丝印 TX / IO9（与 BOOT 共用） */
#define BLINK_ON_MS         90
#define BLINK_OFF_MS        140
#define LED_COUNT_CAP       10     /* 「累计唤醒次数」指示的闪烁上限 */

/* 睡眠前留给日志刷出的时间（USB 会在深睡瞬间断开，日志没刷完就看不到了） */
#define PRE_SLEEP_FLUSH_MS  150

/* RTC 保留内存上下文的魔数（'S','L','P','1'）。用来判断保留内容是否可信。 */
#define SLEEP_CTX_MAGIC     0x31504C53u

/*
 * EXT1 掩码是否把 A0..A5 全部纳入：
 *   1 = 把 A0..A5 都设为唤醒脚（哪根接地就从哪根唤醒，唤醒后打印具体触发脚）
 *   0 = 只用 Kconfig 里 APP_EDK60_SLEEP_EXT1_PIN 指定的那一个脚
 * 两种都能编译、都能跑，切换只需要改这一行。
 */
#define EDK60_EXT1_USE_ALL_A_PINS  1

/* ===========================================================================
 * 引脚表（不硬编码 GPIO 数字，全部取自板级宏）
 * ===========================================================================
 * 丝印归属沿用既有实现 demo_get_started_adc_read.c 的口径：
 * A0/A1 在 H6，A2..A5 在 H3；A2..A5 同时出现在 H4-A（雷达模拟口）上。
 * 最终以 docs/EDK60-pinmap.md 为准。
 */
typedef struct {
    gpio_num_t  pin;
    const char *alias;
    const char *note;
} edk60_ext1_pin_t;

static const edk60_ext1_pin_t s_ext1_pins[] = {
    { EDK60_PIN_A0, "A0", "H6 排针 A0 / ADC1_CH0" },
    { EDK60_PIN_A1, "A1", "H6 排针 A1 / ADC1_CH1" },
    { EDK60_PIN_A2, "A2", "H3 排针 A2 / ADC1_CH2 / 也是 H4-A" },
    { EDK60_PIN_A3, "A3", "H3 排针 A3 / ADC1_CH3 / 也是 H4-A" },
    { EDK60_PIN_A4, "A4", "H3 排针 A4 / ADC1_CH4 / 也是 H4-A / strapping 脚" },
    { EDK60_PIN_A5, "A5", "H3 排针 A5 / ADC1_CH5 / 也是 H4-A / strapping 脚" },
};
#define EXT1_PIN_COUNT  ((int)(sizeof(s_ext1_pins) / sizeof(s_ext1_pins[0])))

/* A0..A5 的 EXT1 掩码：接地其中任意一根都能唤醒。
 * 注意这里是「变量位号 + BSP 宏」的表达方式，不出现裸 GPIO 数字。 */
#define EDK60_EXT1_MASK_A0A5  ((1ULL << EDK60_PIN_A0) | (1ULL << EDK60_PIN_A1) | \
                               (1ULL << EDK60_PIN_A2) | (1ULL << EDK60_PIN_A3) | \
                               (1ULL << EDK60_PIN_A4) | (1ULL << EDK60_PIN_A5))

/* 编译期守住「EXT1 只支持 RTC 域引脚」这条硬约束：
 * 一旦有人把 A0..A5 之外的脚塞进掩码（例如 IO9/IO15），这里会直接编译失败。 */
_Static_assert((EDK60_EXT1_MASK_A0A5 >> (EDK60_EXT1_WAKEUP_GPIO_MAX + 1)) == 0,
               "EXT1 唤醒掩码里出现了 RTC 域（GPIO0..GPIO7）之外的引脚");

/* ===========================================================================
 * RTC 保留内存：深睡周期之间的运行态
 * ===========================================================================
 * 选择理由（为什么是 RTC_DATA_ATTR 而不是 NVS）：
 *   1) RTC 保留内存由 RTC 域供电，深睡期间内容保持，唤醒后立刻可读，**不写 flash**；
 *      NVS 每次写入都要动 flash（页搬移 + 擦除），在「每次深睡都 +1」这种高频场景下
 *      既慢又费电、还消耗 NVS 条目；
 *   2) 本 demo 的这份数据是**运行态**（当前模式、唤醒次数、上次睡眠时间戳），
 *      语义上是「本次上电以来的状态」，用 NVS 的「掉电持久」语义反而会误导；
 *   3) 代价：RTC 保留内存**掉电即丢**（完全断电、长时间掉电），所以唤醒次数是
 *      「自上一次上电复位以来」的计数，不是终身计数。
 *   需要掉电保持的数据（例如复位/启动次数）请用 NVS —— 见 Demo 4.2 nvs_rw。
 */
typedef struct {
    uint32_t magic;               /* == SLEEP_CTX_MAGIC 才认为内容可信 */
    uint32_t mode;                /* 当前唤醒源模式（可在交互窗口里用 BOOT 键切换） */
    uint32_t wake_count;          /* 累计唤醒次数（自本次上电以来） */
    uint32_t reserved;            /* 对齐保留，将来加字段时改 magic 值 */
    uint64_t last_sleep_rtc_us;   /* 上一次进入深睡那一刻的 RTC 计数器快照 */
} sleep_ctx_t;

static RTC_DATA_ATTR sleep_ctx_t s_ctx;

/* 本次启动的运行时上下文（非 RTC，普通 RAM） */
typedef struct {
    bool     from_deep_sleep;     /* 复位原因是 ESP_RST_DEEPSLEEP */
    bool     ctx_valid;           /* RTC 保留内容可信 */
    uint32_t causes;              /* esp_sleep_get_wakeup_causes() 的位图 */
} boot_info_t;

/* 一次睡眠里实际生效的唤醒源（用于打印与降级提示） */
typedef struct {
    bool       timer_enabled;
    bool       ext1_enabled;
    bool       button_enabled;
    bool       button_unsupported;   /* hp_periph_powerdown 返回 ESP_ERR_INVALID_ARG */
    bool       button_other_error;   /* 其它错误（内存/参数等） */
    esp_err_t  button_err;
    gpio_num_t ext1_pin;
    uint64_t   ext1_mask;
} wakeup_cfg_t;

/* ===========================================================================
 * 小工具
 * =========================================================================== */

static int imin(int a, int b)
{
    return (a < b) ? a : b;
}

static uint32_t sleep_timer_sec(void)
{
    uint32_t sec = (uint32_t)CONFIG_APP_EDK60_SLEEP_TIMER_SEC;

    /* 防御：手改 sdkconfig 可能绕过 range 校验，这里兜一层 */
    if (sec < 1 || sec > 3600) {
        sec = 10;
    }
    return sec;
}

static uint32_t clamp_mode(uint32_t mode)
{
    return (mode < SLEEP_MODE_COUNT) ? mode : SLEEP_MODE_TIMER;
}

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
    case SLEEP_MODE_TIMER:  return "TIMER（定时器唤醒）";
    case SLEEP_MODE_EXT1:   return "EXT1（排针 A0..A5 接地唤醒，低电平触发）";
    case SLEEP_MODE_BUTTON: return "BUTTON（BOOT 键 IO9 低电平唤醒）";
    default:                return "未知模式";
    }
}

static const edk60_ext1_pin_t *ext1_pin_lookup(gpio_num_t pin)
{
    for (int i = 0; i < EXT1_PIN_COUNT; i++) {
        if (s_ext1_pins[i].pin == pin) {
            return &s_ext1_pins[i];
        }
    }
    return NULL;
}

/* 把 GPIO 号翻译成「人话」：排针丝印 + 板上用途 */
static void print_pin_name(gpio_num_t pin)
{
    const edk60_ext1_pin_t *d = ext1_pin_lookup(pin);

    if (d != NULL) {
        printf("IO%d（排针 %s；%s）", (int)pin, d->alias, d->note);
        return;
    }
    if (pin == EDK60_PIN_BUTTON_BOOT) {
        printf("IO%d（BOOT 按键 SW2，低电平按下；同时是 LED3/TX 与 strapping 脚）", (int)pin);
        return;
    }
    if (pin == EDK60_PIN_LED_RX) {
        printf("IO%d（LED2 丝印 RX；也是 strapping 脚）", (int)pin);
        return;
    }
    if (pin == EDK60_PIN_RADAR_PWR_EN) {
        printf("IO%d（RADAR_PWR_EN = 电源使能，控制 3.3V_RADAR）", (int)pin);
        return;
    }
    if (pin == EDK60_PIN_RADAR_UART_RX) {
        printf("IO%d（H4-T，雷达 TX → MCU RX）", (int)pin);
        return;
    }
    if (pin == EDK60_PIN_I2C_SDA || pin == EDK60_PIN_I2C_SCL) {
        printf("IO%d（I2C %s）", (int)pin, (pin == EDK60_PIN_I2C_SDA) ? "SDA" : "SCL");
        return;
    }
    printf("IO%d（本板未登记用途的引脚）", (int)pin);
}

static const char *wake_cause_text(uint32_t causes)
{
    if (causes == 0) {
        return "无（本次不是从深睡唤醒的：上电复位 / 外部复位 / 看门狗 / panic）";
    }
    if (causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) {
        return "TIMER —— RTC 定时器到期";
    }
    if (causes & (1UL << ESP_SLEEP_WAKEUP_EXT1)) {
        return "EXT1 —— RTC 外设域唤醒（排针 A0..A5 接地）";
    }
    if (causes & (1UL << ESP_SLEEP_WAKEUP_GPIO)) {
        return "GPIO —— HP 外设掉电域 GPIO 唤醒（本 demo 的 BOOT 键路径）";
    }
    return "其他 / 组合唤醒源（见上方掩码）";
}

/* EXT1 状态掩码：位号 == GPIO 号 */
static void print_ext1_status(uint64_t mask)
{
    if (mask == 0) {
        printf("              （EXT1 掩码为 0：本次不是 EXT1 触发的）\n");
        return;
    }
    for (int n = EDK60_EXT1_WAKEUP_GPIO_MIN; n <= EDK60_EXT1_WAKEUP_GPIO_MAX; n++) {
        /* C6 具备 RTC 功能的脚只有 GPIO0..GPIO7，逐位翻译成排针丝印 */
        if ((mask & (1ULL << n)) == 0) {
            continue;
        }
        printf("              → ");
        print_pin_name((gpio_num_t)n);
        printf("\n");
    }
}

/* GPIO 唤醒状态掩码：位号是 RTC IO 序号，需要用 esp_sleep_wakeup_io_bit2num() 换算 */
static void print_gpio_status(uint64_t rtc_io_mask)
{
    if (rtc_io_mask == 0) {
        printf("              （GPIO 唤醒掩码为 0；本板 IO9 不在 RTC 域，"
               "能拿到非 0 掩码反而说明走通了）\n");
        return;
    }
    for (uint32_t bit = 0; bit < 64; bit++) {
        if ((rtc_io_mask & (1ULL << bit)) == 0) {
            continue;
        }
        gpio_num_t pin = esp_sleep_wakeup_io_bit2num(bit);
        printf("              → RTC IO 位 %" PRIu32 " ⇒ ", bit);
        if (pin == GPIO_NUM_NC) {
            printf("无对应 GPIO\n");
        } else {
            print_pin_name(pin);
            printf("\n");
        }
    }
}

/* ===========================================================================
 * 平台事实与功耗说明（演示材料的一部分，打印一次）
 * =========================================================================== */

static void print_platform_notes(void)
{
    printf("\n[平台事实] ESP32-C6 的深睡唤醒能力（依据 IDF v6.1 头文件，非记忆）：\n");
    printf("  * EXT1（esp_sleep_enable_ext1_wakeup_io）在 C6 上**只支持 GPIO0..GPIO7**\n");
    printf("    （esp_sleep.h 的 io_mask 说明写着 \"ESP32-C6: 0-7\"）。\n");
    printf("  * C6 **没有 EXT0**（soc_caps.h 未定义 SOC_PM_SUPPORT_EXT0_WAKEUP），\n");
    printf("    所以不要用 esp_sleep_enable_ext0_wakeup()。\n");
    printf("  * BOOT 键 = IO%d，**不在 0..7 之内** → 不能用 EXT1 唤醒它；\n",
           EDK60_PIN_BUTTON_BOOT);
    printf("    改用 esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown()，\n");
    printf("    但该 API 同样要求 RTC 域 pad，IO%d 大概率返回 ESP_ERR_INVALID_ARG\n",
           EDK60_PIN_BUTTON_BOOT);
    printf("    —— 遇到就自动降级为定时器兜底，不会睡死。\n");
    printf("  * 本板可用 EXT1 的脚：IO%d..IO%d（排针 A0..A5），默认用 IO%d。\n",
           EDK60_EXT1_WAKEUP_GPIO_MIN, EDK60_EXT1_WAKEUP_GPIO_MAX, (int)EDK60_PIN_A0);
    printf("  * EXT1 低电平唤醒时内部上下拉在 RTC 外设掉电后失效，"
           "实测请给被唤醒脚**外加 10k 上拉到 3.3V**。\n");
    printf("  * 深睡会断开 USB（USB Serial/JTAG 不保活），唤醒是重新枚举串口，"
           "日志从复位重新开始属正常。\n");
}

static void print_power_notes(void)
{
    printf("\n[功耗预期与实测建议]\n");
    printf("  深睡期间 MCU 只占 µA 量级，本板主要看这几路负载"
           "（依据 BSP 2026-10-14 更新后的板级结论）：\n");
    printf("  +----------------------+--------------------------------------------------+\n");
    printf("  | 负载                 | 深睡期间是否耗电 / 说明                          |\n");
    printf("  +----------------------+--------------------------------------------------+\n");
    printf("  | ESP32-C6 深睡        | 是，µA 量级（以 datasheet 的 deep-sleep 表格为准）|\n");
    printf("  | 3.3V LDO 静态电流    | 是，通常几十 µA 量级（查 LDO 器件手册确认）      |\n");
    printf("  | 5V/VIN 输入级        | 是，取决于板上稳压/防反接电路的静态电流          |\n");
    printf("  | LED1 \"ON\"（绿）      | **是**，唯一常亮灯：(3.3V-Vf)/330R；Vf≈2.0V≈3.9mA |\n");
    printf("  | LED4 \"L\"（橙）       | 否，由 IO15 驱动；本 demo 深睡前拉低 → 已熄灭    |\n");
    printf("  | LED5 \"RADAR\"（红）   | 否，挂 3.3V_RADAR；雷达电源关闭 → 已熄灭         |\n");
    printf("  | 雷达模组（若插了）   | 否，3.3V_RADAR 已关断（EN 被 10k 下拉保持低）    |\n");
    printf("  +----------------------+--------------------------------------------------+\n");
    printf("  结论：本 demo 深睡前会调 edk60_radar_power_off()，把雷达 LDO、LED4、LED5\n");
    printf("        一起关掉，因此深睡固定负载只剩 LED1（约几 mA）+ 各稳压器静态电流。\n");
    printf("        注意：**遮盖 LED 不会降低电流**（回路仍导通）；要单独确认 MCU 自身的\n");
    printf("        深睡电流，需临时断开 LED1 支路的 330R，或用未焊常亮灯的对照板做差值。\n");
    printf("  建议步骤：1) 万用表/功耗分析仪串在 VIN(5V) 输入；\n");
    printf("            2) 记录进入深睡后的稳态电流；\n");
    printf("            3) 减去 LED1 与各稳压器静态电流，得到 MCU 近似的深睡电流；\n");
    printf("            4) 对比 datasheet 的 deep-sleep 典型值，偏差大再按外设二分排查。\n");
    printf("  待实测确认的项：① 3.3V_RADAR 关断后整机电流的实际下降量；\n");
    printf("                  ② 各 LED 的真实 Vf 与支路电流（本表按 Vf≈2.0V 估算）；\n");
    printf("                  ③ LED 支路实测电流与 330R 标称值是否一致。\n");
}

/* ===========================================================================
 * NVS 无关：RTC 计数器（用于测深睡时长）
 * =========================================================================== */

static void print_sleep_duration(bool ctx_valid)
{
    if (!ctx_valid) {
        printf("[深睡统计] 本次深睡时长 : 不可测（首次上电，没有上一次深睡的时间戳）\n");
        return;
    }
    if (s_ctx.last_sleep_rtc_us == 0) {
        printf("[深睡统计] 本次深睡时长 : 不可测（RTC 时间戳无效）\n");
        return;
    }

    uint64_t now = esp_rtc_get_time_us();
    if (now <= s_ctx.last_sleep_rtc_us) {
        /* RTC 计数器被复位或回绕：报实测值没有意义，如实说不知道 */
        printf("[深睡统计] 本次深睡时长 : 不可测（RTC 计数器不连续："
               "上次=%llu us 本次=%llu us）\n",
               (unsigned long long)s_ctx.last_sleep_rtc_us, (unsigned long long)now);
        return;
    }

    uint64_t delta_us = now - s_ctx.last_sleep_rtc_us;
    printf("[深睡统计] 本次深睡时长 : %" PRIu32 " ms（%llu us，RTC 计数器实测）\n",
           (uint32_t)(delta_us / 1000ULL), (unsigned long long)delta_us);
    printf("           说明：RTC 计数器跑在 RTC_SLOW_CLK 上（精度取决于 "
           "CONFIG_RTC_CLK_SRC），所以和秒表会有零点几个百分点的偏差。\n");
}

/* ===========================================================================
 * LED 指示
 * =========================================================================== */

/*
 * 准备 LED。
 * 返回 IO9（LED3/TX）是否可用作指示灯。
 *
 * !! 关键安全判断 !!：BUTTON 模式唤醒时用户可能仍按着 BOOT 键（IO9 接地）。
 * 这时如果把 IO9 配成推挽输出并输出高电平，会经按键直接灌到 GND，可能损伤 pad。
 * 因此发现「唤醒瞬间按键已按下」时，本函数**完全不碰 IO9**，只把 IO8 配成输出，
 * 后面所有指示都用 LED2(RX/IO8) 表达。
 */
static bool led_indicator_prepare(void)
{
    bool boot_held = (gpio_get_level(EDK60_PIN_BUTTON_BOOT) == EDK60_BUTTON_ACTIVE_LEVEL);

    if (boot_held) {
        printf("[LED] BOOT 键（IO%d）当前处于按下状态 → **跳过 LED3(TX) 指示**，"
               "只用 LED2(RX/IO%d)。\n"
               "      原因：IO%d 与 BOOT 键共用一个脚，输出高电平会经按键灌到 GND，"
               "可能损伤 pad。\n",
               EDK60_PIN_BUTTON_BOOT, EDK60_PIN_LED_RX, EDK60_PIN_LED_TX);

        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << EDK60_PIN_LED_RX,   /* 只配 IO8，不动 IO9 */
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LED2(IO%d) 配置失败: %s",
                     EDK60_PIN_LED_RX, esp_err_to_name(err));
            return false;
        }
        /* 初始灭（低电平点亮） */
        edk60_led_set(LED_RX_INDEX, false);
        return false;
    }

    /* 按键没被按住：可以安全地把 IO8/IO9 都配成输出 */
    esp_err_t err = edk60_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED 初始化失败: %s", esp_err_to_name(err));
        return false;
    }
    edk60_led_set_all(false);
    return true;
}

static void led_blink(int led_index, int times)
{
    for (int i = 0; i < times; i++) {
        edk60_led_set(led_index, true);
        vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
        edk60_led_set(led_index, false);
        vTaskDelay(pdMS_TO_TICKS(BLINK_OFF_MS));
    }
}

/* 唤醒源编码：1=定时 2=EXT1 3=GPIO(按键) 0=非深睡唤醒 */
static int wake_source_code(uint32_t causes)
{
    if (causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) {
        return 1;
    }
    if (causes & (1UL << ESP_SLEEP_WAKEUP_GPIO)) {
        return 3;
    }
    if (causes & (1UL << ESP_SLEEP_WAKEUP_EXT1)) {
        return 2;
    }
    return 0;
}

static void led_indicate_wake(int src_code, uint32_t wake_count, bool tx_usable)
{
    if (src_code == 0) {
        return;     /* 上电/复位启动，不闪，避免和深睡唤醒混淆 */
    }

    /* 第一段：LED2(RX) 闪「唤醒源编码」次（1/2/3），每次间隔较长便于数 */
    led_blink(LED_RX_INDEX, src_code);

    /* 第二段：闪「累计唤醒次数」次（上限 10），用 LED3(TX)；
     * IO9 不可用时（按键被按住）换成 LED2(RX)，节奏更快以示区别 */
    int times = imin((int)wake_count, LED_COUNT_CAP);
    if (times <= 0) {
        return;
    }
    if (tx_usable) {
        led_blink(LED_TX_INDEX, times);
    } else {
        for (int i = 0; i < times; i++) {
            edk60_led_set(LED_RX_INDEX, true);
            vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS / 2));
            edk60_led_set(LED_RX_INDEX, false);
            vTaskDelay(pdMS_TO_TICKS(BLINK_OFF_MS / 2));
        }
    }
}

/* ===========================================================================
 * 唤醒后的交互窗口（切换模式 / 立即深睡）
 * =========================================================================== */

/*
 * 返回 true = 用户在窗口内长按了 BOOT，要求立刻进入深睡。
 * mode_io 会在短按时被就地切换（并同步到 RTC 保留内存）。
 *
 * wait_release_first：BUTTON/GPIO 唤醒时按键可能还按着，先等松开再接受新按键，
 * 否则会出现「按住 → 唤醒 → 立刻判定长按 → 又深睡 → 立刻再唤醒」的振荡循环。
 */
static bool button_wait_window(uint32_t *mode_io, bool wait_release_first)
{
    esp_err_t err = edk60_button_init_polling();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按键轮询初始化失败: %s（跳过交互窗口）", esp_err_to_name(err));
        return false;
    }

    const int iterations = WAKE_WINDOW_MS / POLL_INTERVAL_MS;
    bool release_needed = wait_release_first && edk60_button_is_pressed();
    bool prev = false;
    uint32_t press_ms = 0;

    if (release_needed) {
        printf("[交互] 唤醒时 BOOT 键仍被按住 → 先等松开再接受新的按键动作"
               "（防止「按住→唤醒→立刻深睡」的振荡循环）\n");
    }

    for (int i = 0; i < iterations; i++) {
        bool now = edk60_button_is_pressed();

        if (release_needed) {
            if (!now) {
                release_needed = false;
                printf("[交互] 已松开。剩余窗口约 %d ms：短按=切换模式，长按>=%d ms=立刻深睡\n",
                       (iterations - i) * POLL_INTERVAL_MS, LONG_PRESS_MS);
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        if (now && !prev) {
            press_ms = 0;                                   /* 新的按下沿 */
        } else if (now) {
            press_ms += POLL_INTERVAL_MS;
            if (press_ms >= LONG_PRESS_MS) {
                printf("[交互] 检测到长按（>= %d ms）→ 立即进入深睡\n", LONG_PRESS_MS);
                return true;
            }
        } else if (!now && prev) {
            if (press_ms < LONG_PRESS_MS) {                 /* 短按 → 切换模式 */
                *mode_io = (*mode_io + 1U) % SLEEP_MODE_COUNT;
                s_ctx.mode = *mode_io;
                printf("[交互] 检测到短按（约 %" PRIu32 " ms）→ 唤醒源切换为 %s\n",
                       press_ms, mode_name(*mode_io));
            }
            press_ms = 0;
        }

        prev = now;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }

    printf("[交互] 窗口结束（%d ms 内没有新的按键动作）\n", WAKE_WINDOW_MS);
    return false;
}

/* ===========================================================================
 * 唤醒源配置（含失败降级）
 * =========================================================================== */

static void wakeup_configure(uint32_t mode, wakeup_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->button_err = ESP_OK;

    /* 显式清掉所有唤醒源：深睡周期里每一步都是新的一次启动，这既是「清标志」的
     * 示范，也避免同一次上电内先后配置多个模式时残留旧配置造成误唤醒。 */
    esp_err_t err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "清理唤醒源失败: %s（继续，后续配置会覆盖）", esp_err_to_name(err));
    }

    switch (mode) {
    case SLEEP_MODE_EXT1: {
        int idx = CONFIG_APP_EDK60_SLEEP_EXT1_PIN;
        if (idx < 0 || idx >= EXT1_PIN_COUNT) {
            ESP_LOGW(TAG, "Kconfig 里的 EXT1 脚序号 %d 越界，回退到 A0", idx);
            idx = 0;
        }
        cfg->ext1_pin = s_ext1_pins[idx].pin;

        /* 该脚必须落在 RTC 域（C6 = GPIO0..GPIO7），否则 EXT1 不可能生效 */
        if ((int)cfg->ext1_pin < EDK60_EXT1_WAKEUP_GPIO_MIN ||
            (int)cfg->ext1_pin > EDK60_EXT1_WAKEUP_GPIO_MAX) {
            ESP_LOGE(TAG, "IO%d 不在 C6 的 RTC 域（GPIO%d..GPIO%d）内，EXT1 无法使用",
                     (int)cfg->ext1_pin, EDK60_EXT1_WAKEUP_GPIO_MIN, EDK60_EXT1_WAKEUP_GPIO_MAX);
            break;
        }

#if EDK60_EXT1_USE_ALL_A_PINS
        cfg->ext1_mask = EDK60_EXT1_MASK_A0A5;
#else
        cfg->ext1_mask = (1ULL << cfg->ext1_pin);
#endif

        err = esp_sleep_enable_ext1_wakeup_io(cfg->ext1_mask, ESP_EXT1_WAKEUP_ANY_LOW);
        if (err == ESP_OK) {
            cfg->ext1_enabled = true;
        } else {
            ESP_LOGE(TAG, "EXT1 唤醒配置失败: %s（掩码 0x%08llX）",
                     esp_err_to_name(err), (unsigned long long)cfg->ext1_mask);
        }
        break;
    }

    case SLEEP_MODE_BUTTON: {
        /* IO9 不在 RTC 域，这个 API 预期会返回 ESP_ERR_INVALID_ARG。
         * 允许失败 → 失败就降级到定时器兜底，绝不让板子睡死。 */
        err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
                  (1ULL << EDK60_PIN_BUTTON_BOOT), ESP_GPIO_WAKEUP_GPIO_LOW);
        cfg->button_err = err;

        if (err == ESP_OK) {
            cfg->button_enabled = true;
        } else if (err == ESP_ERR_INVALID_ARG) {
            cfg->button_unsupported = true;
        } else {
            cfg->button_other_error = true;
        }
        break;
    }

    case SLEEP_MODE_TIMER:
    default:
        err = esp_sleep_enable_timer_wakeup((uint64_t)sleep_timer_sec() * 1000000ULL);
        if (err == ESP_OK) {
            cfg->timer_enabled = true;
        } else {
            ESP_LOGE(TAG, "定时器唤醒配置失败: %s", esp_err_to_name(err));
        }
        break;
    }

    /* 兜底：任何模式只要一个唤醒源都没配成功，就加一个定时器唤醒。
     * 绝不让板子在「零唤醒源」的状态下深睡 —— 那样只有按 RESET 键才能救回来，
     * 对演示板来说是最糟糕的失败模式。（BUTTON 返回 ESP_ERR_INVALID_ARG 就走这里） */
    if (!cfg->timer_enabled && !cfg->ext1_enabled && !cfg->button_enabled) {
        uint32_t sec = sleep_timer_sec();
        err = esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
        if (err == ESP_OK) {
            cfg->timer_enabled = true;
        } else {
            ESP_LOGE(TAG, "兜底定时器也配置失败: %s —— 本次深睡后只能靠 RESET 键唤醒！",
                     esp_err_to_name(err));
        }
    }
}

static void print_wakeup_plan(uint32_t mode, const wakeup_cfg_t *cfg)
{
    printf(" 唤醒源     : %s\n", mode_name(mode));

    if (cfg->timer_enabled) {
        printf("              → 定时器已使能：%" PRIu32 " 秒后自动唤醒\n", sleep_timer_sec());
    }
    if (cfg->ext1_enabled) {
        printf("              → EXT1 已使能：掩码 0x%08llX，任一脚接地（低电平）即唤醒。"
               "默认关注 ", (unsigned long long)cfg->ext1_mask);
        if ((int)cfg->ext1_pin >= EDK60_EXT1_WAKEUP_GPIO_MIN &&
            (int)cfg->ext1_pin <= EDK60_EXT1_WAKEUP_GPIO_MAX) {
            print_pin_name(cfg->ext1_pin);
        } else {
            printf("（无有效引脚）");
        }
        printf("\n                  提示：请在 A0..A5 里选一根跳线到 GND；"
               "内部上下拉在 RTC 外设掉电后失效，实测请外加 10k 上拉到 3.3V。\n");
    }
    if (cfg->button_enabled) {
        printf("              → BOOT 按键唤醒已使能：");
        print_pin_name(EDK60_PIN_BUTTON_BOOT);
        printf(" 低电平触发\n");
        printf("                 说明：板上 IO%d 已有 4.7k 外部上拉；IDF 默认还会打开内部\n",
               EDK60_PIN_BUTTON_BOOT);
        printf("                 上拉（menuconfig 的 ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS，\n");
        printf("                 见 esp_sleep.h 注释）。若唤醒不稳定，可关掉该项排查。\n");
    }
    if (cfg->button_unsupported) {
        printf("              !! 降级：esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown()\n");
        printf("                 返回 ESP_ERR_INVALID_ARG（错误码 %d = %s）。\n",
               (int)cfg->button_err, esp_err_to_name(cfg->button_err));
        printf("                 原因：该 API 的掩码同样要求 RTC 域 pad（VDD3P3_RTC 供电），\n");
        printf("                 而 IO%d（BOOT）是数字 pad，不在 0..7 之内 → 不支持。\n",
               EDK60_PIN_BUTTON_BOOT);
        printf("                 降级方案：本次改用**定时器唤醒**（%"
               PRIu32 " 秒），"
               "想演示「接地唤醒」请把 A0..A5 之一跳线到 GND 并切到 EXT1 模式。\n",
               sleep_timer_sec());
    } else if (cfg->button_other_error) {
        printf("              !! BOOT 唤醒配置返回其它错误: %s（错误码 %d）→ 定时器兜底\n",
               esp_err_to_name(cfg->button_err), (int)cfg->button_err);
    }
    if (!cfg->timer_enabled && !cfg->ext1_enabled && !cfg->button_enabled) {
        printf("              !! 没有任何唤醒源配置成功：本次深睡后只能靠 RESET 键唤醒！\n");
    }
}

/* ===========================================================================
 * 深睡前：打印 + 刷日志 + 关机电源提示
 * =========================================================================== */

static void prepare_and_enter_deep_sleep(uint32_t mode, const wakeup_cfg_t *cfg)
{
    printf("\n================ 即将进入深睡 ================\n");
    print_wakeup_plan(mode, cfg);

    printf(" 预计唤醒   : ");
    if (cfg->timer_enabled && !cfg->ext1_enabled && !cfg->button_enabled) {
        printf("约 %" PRIu32 " 秒后（定时器）\n", sleep_timer_sec());
    } else if (cfg->ext1_enabled || cfg->button_enabled) {
        printf("无定时器约束，等外部低电平触发（EXT1 排针接地 / BOOT 键）——\n");
        printf("              **没有超时兜底**：只要不触发就一直睡着。"
               "量产固件建议同时使能一个兜底定时器。\n");
    } else {
        printf("未知（本次没有可用的唤醒源）\n");
    }

    print_power_notes();

    /* --- 1. 关掉 3.3V_RADAR（雷达 LDO）—— 本 demo 的核心低功耗优化点 ---
     *
     * 板级事实（依据 components/edk60_board/include/edk60_board.h，2026-10-14
     * 逐层读图核实的结论；旧版本记的「10k 上拉、上电默认开、开漏拉低」是错的）：
     *   链路 1（使能）： U4(AP7362-33SP-13).EN = **IO14**
     *                    （2026-10-15 上板实测定论；早期读图推断为 IO15 是错的）
     *                    ⇒ 复位后 EN 被 R2 下拉到 GND ⇒ **雷达电源默认关闭**；
     *                    ⇒ 使能 = IO14 输出高（推挽即可）。
     *   链路 2（指示）： IO15 ── R9(330Ω) ── LED4（橙，丝印 "L"） 高电平点亮，
     *                    **仅指示灯，与电源 EN 无关**；
     *                    LED5（红，丝印 "RADAR"）挂在 3.3V_RADAR 输出端，
     *                    它才是「雷达是否真的有电」的肉眼判据。
     *   ⇒ 关掉雷达电源会熄灭 LED5；LED4 是否亮只反映 IO15 电平。
     *
     * 深睡期间会怎样：IO15 不是 RTC 域 pad（C6 的 RTC 脚只有 GPIO0..7），深睡时
     * HP 域掉电、pad 不再驱动；但这里**不需要 pad 保持**——外部 R2 这 10k 下拉
     * 会把 EN 一直拉到 GND，所以 **3.3V_RADAR 在深睡期间保持关闭是硬件保证的**，
     * 不依赖芯片是否 hold 住引脚电平（这正是本板设计的巧妙处）。
     * （作为对照：若 EN 是上拉结构就需要 pad 保持，那种情况下非 RTC 脚做不到，
     *   必须改硬件或用 gpio_hold_en()——本板不属于这种情况，无需该措施。）
     *
     * IO15 同时是 C6 的 strapping 脚（EDK60_PIN_STRAP_15），但本板上电默认电平
     * 本来就是低（10k 下拉），所以深睡期间保持低**并没有引入新的 strapping 状态**，
     * 风险与普通上电相同（具体 strapping 含义请查 C6 datasheet 的 strapping 表）。
     *
     * 仍需上板实测确认的项：关断 3.3V_RADAR 后整机静态电流实际下降了多少
     * （板上还有 LED1、各稳压器静态电流等固定负载），本 demo 不臆造具体数字。 */
    esp_err_t err = edk60_radar_power_off();
    if (err == ESP_OK) {
        printf(" 雷达电源   : 已关闭 3.3V_RADAR（IO%d 输出低；LED4 与 LED5 同步熄灭）\n",
               EDK60_PIN_RADAR_PWR_EN);
        printf("              深睡期间由外部 R2(10k 下拉) 保持 EN=0 ⇒ "
               "雷达电源保持关闭（硬件保证，不依赖引脚 hold）\n");
    } else {
        printf(" 雷达电源   : 关闭失败（%s）→ 3.3V_RADAR 会用硬件默认值（默认关闭）\n",
               esp_err_to_name(err));
    }

    /* --- 2. LED 灭（避免人为负载） ---
     * IO8/IO9 两颗可以程序熄灭；LED1 "ON"（绿，3.3V 直供）深睡期间照旧亮，
     * 这是本板深睡固定负载的一部分，无法用软件关掉。 */
    edk60_led_set_all(false);

    printf(" USB 提示   : 深睡会断开 USB（USB Serial/JTAG 不保活）。唤醒后主机需要\n");
    printf("              重新枚举串口，日志表现为「从复位重新开始」，属正常现象。\n");
    printf("================================================\n\n");

    /* --- 3. 记录 RTC 时间戳（唤醒后用来算实际深睡时长） ---
     * 必须在 esp_deep_sleep_start() 之前写进 RTC 保留内存。 */
    s_ctx.last_sleep_rtc_us = esp_rtc_get_time_us();

    /* --- 4. 保证日志刷出 --- */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(PRE_SLEEP_FLUSH_MS));

    esp_deep_sleep_start();     /* 不返回 */

    /* 正常情况下永远走不到这里：esp_deep_sleep_start() 会复位芯片 */
    ESP_LOGE(TAG, "esp_deep_sleep_start() 意外返回（理论上不会发生）");
}

/* ===========================================================================
 * 主流程
 * =========================================================================== */

static void sleep_demo_task(void *arg)
{
    (void)arg;

    boot_info_t boot = {
        .from_deep_sleep = (esp_reset_reason() == ESP_RST_DEEPSLEEP),
        .ctx_valid = false,
        .causes = esp_sleep_get_wakeup_causes(),
    };

    print_platform_notes();

    /* 雷达电源控制脚先初始化：BSP 会把它配成推挽输出并置低，
     * 也就是「默认关闭」（与 R2 10k 下拉的硬件默认一致），不会给雷达上电。
     * 初始化失败不影响本 demo 主体流程，只提示。 */
    if (edk60_radar_power_init() != ESP_OK) {
        ESP_LOGW(TAG, "雷达电源控制脚初始化失败，跳过深睡前的关断尝试");
    } else {
        printf("[雷达电源] IO%d 初始电平 = %d（1 = 3.3V_RADAR 已使能；"
               "初始化后为 0 = 关闭，与硬件 10k 下拉默认一致）\n",
               EDK60_PIN_RADAR_PWR_EN, (int)edk60_radar_power_is_on());
    }

#if !CONFIG_APP_EDK60_STARTUP_BANNER
    /* 启动横幅被关掉时 app_main 不会打印复位原因，这里补一次。
     * 横幅（默认开）已经调过 edk60_print_reset_reason()，其中就包含深睡唤醒原因，
     * 所以这里不重复调用，避免同一段日志打两遍。 */
    edk60_print_reset_reason();
#endif

    /* ---------- 1. 恢复 / 重建 RTC 保留上下文 ---------- */
    boot.ctx_valid = boot.from_deep_sleep && (s_ctx.magic == SLEEP_CTX_MAGIC);

    if (!boot.ctx_valid) {
        /* 上电复位 / 外部复位 / 看门狗 / panic：RTC 保留内容不可信，整体重建 */
        memset(&s_ctx, 0, sizeof(s_ctx));
        s_ctx.magic = SLEEP_CTX_MAGIC;
        s_ctx.mode = clamp_mode((uint32_t)CONFIG_APP_EDK60_SLEEP_MODE);
        s_ctx.wake_count = 0;
        s_ctx.last_sleep_rtc_us = 0;
    } else {
        s_ctx.wake_count++;
    }

    uint32_t mode = clamp_mode(s_ctx.mode);

    /* ---------- 2. 唤醒原因与统计 ---------- */
    printf("\n================ 深睡唤醒 / 启动信息 ================\n");
    printf("[唤醒原因] %s\n", wake_cause_text(boot.causes));
    printf("[唤醒掩码] causes = 0x%08" PRIX32 "\n", boot.causes);

#if SOC_PM_SUPPORT_EXT1_WAKEUP
    if (boot.causes & (1UL << ESP_SLEEP_WAKEUP_EXT1)) {
        printf("[EXT1 触发脚]\n");
        print_ext1_status(esp_sleep_get_ext1_wakeup_status());
    }
#endif
#if SOC_GPIO_SUPPORT_HP_PERIPH_PD_SLEEP_WAKEUP
    if (boot.causes & (1UL << ESP_SLEEP_WAKEUP_GPIO)) {
        printf("[GPIO 触发脚]\n");
        print_gpio_status(esp_sleep_get_gpio_wakeup_status());
    }
#endif

    print_sleep_duration(boot.ctx_valid);

    if (boot.ctx_valid) {
        printf("[深睡统计] 累计唤醒次数 : %" PRIu32 " 次（自本次上电以来，存 RTC 保留内存）\n",
               s_ctx.wake_count);
        printf("           本次是本次上电后的第 %" PRIu32 " 次运行（首次启动计为第 1 次）\n",
               s_ctx.wake_count + 1U);
    } else {
        printf("[深睡统计] 累计唤醒次数 : 0（本次是上电/复位后的首次运行；"
               "RTC 保留内存已重置）\n");
    }
    printf("[当前模式] %s\n", mode_name(mode));
    printf("           （Kconfig APP_EDK60_SLEEP_MODE=%d；"
           "唤醒后 3 秒内短按 BOOT 可循环切换，长按 >=%d ms 立即再深睡）\n",
           CONFIG_APP_EDK60_SLEEP_MODE, LONG_PRESS_MS);
    printf("================================================\n\n");

    /* ---------- 3. LED 指示（必须在按键初始化之前做完） ---------- */
    bool tx_usable = led_indicator_prepare();
    led_indicate_wake(wake_source_code(boot.causes), s_ctx.wake_count, tx_usable);

    /* ---------- 4. 交互窗口：切换模式 / 长按立即深睡 ---------- */
    bool forced = button_wait_window(&mode, boot.from_deep_sleep);
    if (forced) {
        printf("[交互] 用户要求立即深睡（不再等满窗口）\n");
    }
    mode = clamp_mode(mode);
    s_ctx.mode = mode;

    /* ---------- 5. 配置唤醒源 ---------- */
    wakeup_cfg_t cfg;
    wakeup_configure(mode, &cfg);

    /* ---------- 6. 打印睡眠计划 + 进入深睡（不返回） ---------- */
    prepare_and_enter_deep_sleep(mode, &cfg);
}

void demo_deep_sleep_start(void)
{
    ESP_LOGI(TAG, "=== deep_sleep：ESP32-C6 深睡 + 定时 / EXT1 排针 / BOOT 按键 三种唤醒 ===");
    ESP_LOGI(TAG, "Kconfig 缺省模式=%d（0=定时 1=EXT1 排针 2=BOOT 键），定时周期=%d 秒，EXT1 脚序号=%d",
             CONFIG_APP_EDK60_SLEEP_MODE, CONFIG_APP_EDK60_SLEEP_TIMER_SEC,
             CONFIG_APP_EDK60_SLEEP_EXT1_PIN);

    /* 任务栈 4096（要求 >= 4096）：本 demo 要做 printf/日志与按键轮询，
     * 不用 app_main 的默认栈，避免深睡前的日志把主任务栈顶穿。 */
    BaseType_t ok = xTaskCreate(sleep_demo_task, "sleep_demo", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败（内存不足），deep_sleep demo 无法启动");
    }
}
