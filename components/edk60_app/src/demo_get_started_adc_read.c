/*
 * EDK60 Demo 1.5 —— adc_read
 *
 * 目的：验证 ADC1 六个通道的采集与校准（把原始码值换算成毫伏）。
 *
 * 硬件事实（原理图 H3/H6 + R43..R47）：
 *   A0=IO0  A1=IO1  A2=IO2  A3=IO3  A4=IO4  A5=IO5，全部落在 ADC1，
 *   每路串 10R 隔离电阻。IO6（H1 的 SDA）是 ADC1_CH6，但常用于 I2C，
 *   本 demo 默认跳过，可用接线后通过 config 打开（见代码注释）。
 *   IO7（SCL）在 C6 上没有 ADC 功能。
 *
 * 参考电压与量程：衰减 12dB 时可用量程约 0~3.3V（曲线拟合校准按此建模）。
 *   !! 不要往这些脚灌超过 3.3V 的电压，C6 的 Vin max = 3.6V(绝对最大)。
 *
 * 行为：
 *   - 每 1 秒扫描 A0..A5，打印 raw / 校准后毫伏 / 折合电压；
 *   - 同时输出一行 CSV（A0,A1,...,A5 的毫伏值）便于直接用串口示波器画图；
 *   - 分压公式：若被测电压超过 3.3V，请自行在外部分压，并把系数填到 DIVIDER。
 */

#include <inttypes.h>
#include <stdio.h>

#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo_adc";

/* 采样配置 */
#define ADC_ATTEN       ADC_ATTEN_DB_12   /* 约 0 ~ 3.3V */
#define ADC_BITWIDTH    ADC_BITWIDTH_12   /* 0 ~ 4095 */
#define ADC_SAMPLES     16                /* 每次取 16 次平均，抑制噪声 */
#define ADC_PERIOD_MS   1000

/* 若外部加了分压网络（例如 1/2 分压测 0~6V），把系数改成 2.0 */
#define ADC_DIVIDER     1.0f

typedef struct {
    gpio_num_t pin;
    adc_channel_t channel;
    const char *label;
} adc_pin_desc_t;

static const adc_pin_desc_t s_channels[EDK60_ADC_CHANNEL_COUNT] = {
    { EDK60_PIN_A0, EDK60_ADC_CH_A0, "A0/H6" },
    { EDK60_PIN_A1, EDK60_ADC_CH_A1, "A1/H6" },
    { EDK60_PIN_A2, EDK60_ADC_CH_A2, "A2/H3" },
    { EDK60_PIN_A3, EDK60_ADC_CH_A3, "A3/H3" },
    { EDK60_PIN_A4, EDK60_ADC_CH_A4, "A4/H3" },
    { EDK60_PIN_A5, EDK60_ADC_CH_A5, "A5/H3" },
};

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali[EDK60_ADC_CHANNEL_COUNT];
static bool s_cali_ok[EDK60_ADC_CHANNEL_COUNT];

static bool adc_calibration_init(adc_unit_t unit, adc_channel_t chan,
                                 adc_atten_t atten, adc_bitwidth_t width,
                                 adc_cali_handle_t *out)
{
    *out = NULL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit,
        .chan = chan,
        .atten = atten,
        .bitwidth = width,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, out) == ESP_OK) {
        ESP_LOGI(TAG, "通道 CH%d 使用曲线拟合校准", chan);
        return true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t lcfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = width,
    };
    if (adc_cali_create_scheme_line_fitting(&lcfg, out) == ESP_OK) {
        ESP_LOGW(TAG, "通道 CH%d 曲线拟合不可用，退化为线性校准", chan);
        return true;
    }
#endif

    ESP_LOGW(TAG, "通道 CH%d 无可用校准方案，将只输出原始码值", chan);
    return false;
}

static int adc_read_averaged(adc_channel_t chan, int *out_raw)
{
    int32_t sum = 0;
    int valid = 0;
    int raw = 0;

    for (int i = 0; i < ADC_SAMPLES; i++) {
        if (adc_oneshot_read(s_unit, chan, &raw) == ESP_OK) {
            sum += raw;
            valid++;
        }
    }
    if (valid == 0) {
        *out_raw = 0;
        return -1;
    }
    *out_raw = (int)(sum / valid);
    return *out_raw;
}

static void adc_task(void *arg)
{
    (void)arg;
    static int mv[EDK60_ADC_CHANNEL_COUNT];

    while (1) {
        printf("\n[ADC1 扫描] 衰减=12dB 位宽=12bit 平均=%d 次 分压=%.2fx\n",
               ADC_SAMPLES, (double)ADC_DIVIDER);

        for (int i = 0; i < EDK60_ADC_CHANNEL_COUNT; i++) {
            int raw = 0;
            if (adc_read_averaged(s_channels[i].channel, &raw) < 0) {
                printf("  %-6s IO%-2d  CH%d  读取失败\n",
                       s_channels[i].label, s_channels[i].pin, s_channels[i].channel);
                mv[i] = 0;
                continue;
            }

            int voltage = 0;
            if (s_cali_ok[i] && s_cali[i]) {
                if (adc_cali_raw_to_voltage(s_cali[i], raw, &voltage) != ESP_OK) {
                    voltage = 0;
                }
            }
            mv[i] = voltage;

            printf("  %-6s IO%-2d  CH%d  raw=%4d  校准=%4d mV  折合=%5.3f V\n",
                   s_channels[i].label, s_channels[i].pin, s_channels[i].channel,
                   raw, voltage, (double)voltage * ADC_DIVIDER / 1000.0);
        }

        /* 串口示波器（Arduino Serial Plotter）可直接吃的一行 CSV */
        printf("CSV_A0_A5_mV: %d,%d,%d,%d,%d,%d\n",
               mv[0], mv[1], mv[2], mv[3], mv[4], mv[5]);

        vTaskDelay(pdMS_TO_TICKS(ADC_PERIOD_MS));
    }
}

void demo_adc_read_start(void)
{
    ESP_LOGI(TAG, "=== adc_read：ADC1 六通道扫描 ===");
    ESP_LOGI(TAG, "通道映射：A0=IO%d A1=IO%d A2=IO%d A3=IO%d A4=IO%d A5=IO%d（均经 10R 隔离）",
             EDK60_PIN_A0, EDK60_PIN_A1, EDK60_PIN_A2, EDK60_PIN_A3, EDK60_PIN_A4, EDK60_PIN_A5);
    ESP_LOGI(TAG, "提示：悬空脚会读到浮空噪声，属正常；测电压请共地，勿超过 3.3V。");

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = EDK60_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_unit));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH,
    };

    for (int i = 0; i < EDK60_ADC_CHANNEL_COUNT; i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_unit, s_channels[i].channel, &chan_cfg));
        s_cali_ok[i] = adc_calibration_init(EDK60_ADC_UNIT, s_channels[i].channel,
                                            ADC_ATTEN, ADC_BITWIDTH, &s_cali[i]);
    }

    xTaskCreate(adc_task, "adc", 4096, NULL, 4, NULL);
}


