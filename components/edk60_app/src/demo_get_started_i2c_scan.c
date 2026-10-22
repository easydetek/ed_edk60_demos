/*
 * EDK60 Demo 3.3 —— i2c_scan
 *
 * 目的：验证 I2C 总线（H1 排针的 SDA/SCL）能用，并识别挂在上面的从机。
 *
 * 硬件事实（原理图 H1 网络标号 SDA/IO6 与 SCL/IO7）：
 *   IO6 = SDA（同时是 ADC1_CH6，即 H1 的 AREF 相邻脚；做 I2C 时不能再当 ADC 用）
 *   IO7 = SCL（C6 上 IO7 没有 ADC 功能）
 *   两者都是 LP_I2C 的默认脚，走 10R 隔离电阻引到排针。
 *
 * !! 板上没有任何板载 I2C 器件 —— 必须外接器件（传感器/OLED/EEPROM）才有扫描结果。
 *    因此「扫不到任何地址」在空板情况下是正常结果，不是失败。
 *    内建弱上拉在 100kHz 下勉强可用；挂多个器件或长线时请外接 4.7k 上拉到 3.3V。
 *
 * 行为：
 *   1. 扫描 7bit 地址 0x03..0x77，逐地址探测（i2c_master_probe），打印 ASCII 表格；
 *   2. 对识别到的常见器件给出型号提示（内置一张常见地址→器件对照表）；
 *   3. 发现器件后，对每个器件尝试做一次「读 1 字节」（不写寄存器地址，直接读），
 *      成功则打印首字节，用于快速确认器件是否真的应答数据；
 *   4. 每 10 秒自动重扫一次，方便边接线边看结果；按键（BOOT）可立即触发重扫。
 *
 * 观察点：
 *   - 空板：日志显示 "未发现任何 I2C 器件"，并给出接线与上拉提示；
 *   - 接了器件：表格里出现地址（如 0x3C = SSD1306 OLED、0x76 = BME280）；
 *   - 接线错误/无上拉：全部地址无应答，日志会提示常见原因。
 */

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "demo_i2c";

#define I2C_SCAN_ADDR_MIN   0x03
#define I2C_SCAN_ADDR_MAX   0x77
#define I2C_SCAN_TIMEOUT_MS 50
#define I2C_SCAN_PERIOD_MS  10000

/* 常见 I2C 器件地址对照（用于把扫描结果翻译成人话） */
typedef struct {
    uint8_t addr;
    const char *name;
} i2c_known_t;

static const i2c_known_t s_known[] = {
    { 0x0A, "通用（TI/ST 部分传感器默认地址）" },
    { 0x1E, "HMC5883L / LSM303 磁力计" },
    { 0x20, "PCF8574 或 TCA9555 IO 扩展" },
    { 0x21, "PCF8574 IO 扩展 / TCA9555" },
    { 0x22, "PCF8574 IO 扩展" },
    { 0x23, "BH1750 光照传感器" },
    { 0x24, "PCF8574 IO 扩展" },
    { 0x25, "PCF8574 IO 扩展" },
    { 0x27, "PCF8574 LCD1602 背包 / PCF8574A" },
    { 0x29, "TSL2561 光照 / VL53L0X 激光测距" },
    { 0x38, "AHT10/AHT20 温湿度（部分批次 0x38）" },
    { 0x39, "TSL2561 光照 / APDS-9960" },
    { 0x3C, "SSD1306 / SH1106 OLED（128x64，最常见）" },
    { 0x3D, "SSD1306 OLED（备用地址）" },
    { 0x40, "INA219 电流检测 / HTU21D / SHT21" },
    { 0x44, "SHT31 温湿度 / SGP30 空气质量" },
    { 0x45, "SHT31 温湿度（备用）" },
    { 0x48, "ADS1115 ADC / LM75 温度 / PCF8591" },
    { 0x49, "ADS1115 ADC（备用）/ TMP102" },
    { 0x4A, "TMP102 温度" },
    { 0x50, "AT24Cxx EEPROM" },
    { 0x51, "AT24Cxx EEPROM / PCF8563 RTC" },
    { 0x52, "AT24Cxx EEPROM / MPU-9250 磁力计" },
    { 0x53, "ADXL345 加速度计 / AT24Cxx" },
    { 0x57, "AT24Cxx EEPROM / MAX30102 血氧" },
    { 0x5A, "CCS811 空气质量 / MLX90614 红外测温" },
    { 0x5C, "AM2315 / CAP1188 触摸" },
    { 0x60, "MPL3115A2 气压 / Si1145" },
    { 0x63, "Si7021 / SHT31 兼容" },
    { 0x68, "DS1307/DS3231 RTC / MPU-6050 六轴" },
    { 0x69, "MPU-6050（备用地址）/ ITG-3200" },
    { 0x6A, "LSM6DS3 / L3GD20 陀螺仪" },
    { 0x6B, "LSM6DS3 陀螺仪（备用）" },
    { 0x76, "BME280 / BMP280 气压温湿度（最常见）" },
    { 0x77, "BME280/BMP280（备用）/ BME680" },
};

static const char *lookup_known(uint8_t addr)
{
    for (size_t i = 0; i < sizeof(s_known) / sizeof(s_known[0]); i++) {
        if (s_known[i].addr == addr) {
            return s_known[i].name;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * 扫描
 * ------------------------------------------------------------------------- */

static int scan_once(edk60_i2c_bus_handle_t bus)
{
    int found = 0;

    printf("\n[I2C 扫描] SDA=IO%d  SCL=IO%d  %d Hz  地址范围 0x%02X~0x%02X\n",
           EDK60_PIN_I2C_SDA, EDK60_PIN_I2C_SCL,
           EDK60_I2C_DEFAULT_FREQ_HZ, I2C_SCAN_ADDR_MIN, I2C_SCAN_ADDR_MAX);
    printf("      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");

    for (uint8_t row = 0; row < 8; row++) {
        printf("0x%02X ", row * 16);
        for (uint8_t col = 0; col < 16; col++) {
            uint8_t addr = row * 16 + col;
            if (addr < I2C_SCAN_ADDR_MIN || addr > I2C_SCAN_ADDR_MAX) {
                printf("   ");
                continue;
            }
            bool present = false;
            edk60_i2c_probe(bus, addr, &present);
            if (present) {
                printf(" %02X", addr);
                found++;
            } else {
                printf(" --");
            }
        }
        printf("\n");
    }

    if (found == 0) {
        printf("[I2C 扫描] 未发现任何 I2C 器件。\n");
        printf("  说明：EDK60 板上**没有**板载 I2C 器件，必须从 H1 排针外接（SDA=IO%d, SCL=IO%d, 3V3, GND）。\n",
               EDK60_PIN_I2C_SDA, EDK60_PIN_I2C_SCL);
        printf("  常见原因：①SDA/SCL 接反；②未接 3.3V 或未共地；③器件需要外部 4.7k 上拉；\n");
        printf("            ④器件地址不在 0x03~0x77（极少数 10bit 地址器件）；⑤器件本身未使能/未上电。\n");
    } else {
        printf("[I2C 扫描] 共发现 %d 个器件：\n", found);
        for (uint8_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; addr++) {
            bool present = false;
            edk60_i2c_probe(bus, addr, &present);
            if (!present) {
                continue;
            }
            const char *name = lookup_known(addr);
            printf("  - 0x%02X  %s\n", addr, name ? name : "未知器件（请查其 datasheet）");
        }
    }
    return found;
}

/**
 * 对已发现的地址尝试一次「裸读 1 字节」：
 * 有些器件（例如 OLED）只支持写、直接读会 NACK，这属正常；
 * 这里只是给出「读通路是否可用」的额外线索，不作为成败判据。
 */
static void try_read_probe(edk60_i2c_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = EDK60_I2C_DEFAULT_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device((i2c_master_bus_handle_t)bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        printf("    0x%02X 读测试：无法挂载设备（%s）\n", addr, esp_err_to_name(err));
        return;
    }

    uint8_t byte = 0;
    err = i2c_master_receive(dev, &byte, 1, I2C_SCAN_TIMEOUT_MS);
    if (err == ESP_OK) {
        printf("    0x%02X 读测试：成功，首字节 = 0x%02X\n", addr, byte);
    } else {
        printf("    0x%02X 读测试：NACK/失败（%s）—— 只写器件（如 OLED）属正常\n",
               addr, esp_err_to_name(err));
    }
    i2c_master_bus_rm_device(dev);
}

/* ---------------------------------------------------------------------------
 * 任务
 * ------------------------------------------------------------------------- */

static volatile bool s_rescan_request;

static void on_button(edk60_button_event_t event, uint32_t hold_ms, void *ctx)
{
    (void)hold_ms;
    (void)ctx;
    if (event == EDK60_BUTTON_EVENT_PRESSED) {
        s_rescan_request = true;
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    edk60_i2c_bus_handle_t bus = NULL;
    esp_err_t err = edk60_i2c_init(&bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s（请检查 IO%d/IO%d 是否被外部电路拉死）",
                 esp_err_to_name(err), EDK60_PIN_I2C_SDA, EDK60_PIN_I2C_SCL);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        s_rescan_request = false;
        int found = scan_once(bus);

        /* 对每个发现的地址做一次读通路测试 */
        for (uint8_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; addr++) {
            bool present = false;
            edk60_i2c_probe(bus, addr, &present);
            if (present) {
                try_read_probe(bus, addr);
            }
        }

        if (found > 0) {
            edk60_led_set(0, true);   /* LED_RX 亮 = 总线上有器件 */
        } else {
            edk60_led_set(0, false);
        }

        /* 等待：可被按键提前唤醒重扫 */
        for (int waited = 0; waited < I2C_SCAN_PERIOD_MS; waited += 100) {
            if (s_rescan_request) {
                ESP_LOGI(TAG, "按键请求重扫");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */

void demo_i2c_scan_start(void)
{
    ESP_LOGI(TAG, "=== i2c_scan：I2C 总线扫描 ===");
    ESP_LOGI(TAG, "SDA=IO%d，SCL=IO%d，%d Hz，每 %d 秒自动重扫（按 BOOT 键可立即重扫）",
             EDK60_PIN_I2C_SDA, EDK60_PIN_I2C_SCL,
             EDK60_I2C_DEFAULT_FREQ_HZ, I2C_SCAN_PERIOD_MS / 1000);
    ESP_LOGI(TAG, "注意：板上无板载 I2C 器件，需从 H1 排针外接；"
                  "100kHz 下靠内部弱上拉勉强可用，接线较长时请外接 4.7k 上拉到 3.3V。");
    ESP_LOGI(TAG, "另注意：IO%d 同时是 ADC1_CH6，做 I2C 后不能再当模拟输入用。", EDK60_PIN_I2C_SDA);

    ESP_ERROR_CHECK(edk60_led_init());
    ESP_ERROR_CHECK(edk60_button_init_polling());
    ESP_ERROR_CHECK(edk60_button_init_isr(on_button, NULL));

    xTaskCreate(scan_task, "i2c_scan", 4096, NULL, 4, NULL);
}
