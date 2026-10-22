/*
 * EDK60 Demo 4.2 —— nvs_rw（NVS 读写 / 统计 / 键遍历 / 受控擦除）
 *
 * 目的
 * ----
 *   1) 演示 NVS 的初始化与「分区异常」的正确处理姿势（默认**不**静默擦除）；
 *   2) 演示 string / i32 / u32 / blob 四类数据的读写，并打印「旧值 → 新值」对比；
 *   3) 用一个「数据格式版本号（fmt_ver）」演示数据迁移的基本做法；
 *   4) 用启动计数（boot_cnt）给出很有用的板级自检信息：「本机第 N 次启动」；
 *   5) 演示 nvs_get_stats() 分区统计与 nvs_entry_find_in_handle() 键遍历；
 *   6) 用 BOOT 键做交互，并示范「破坏性操作」的正确做法：
 *      默认关闭 + 编译期开关 + 运行期二次确认 + 只作用于自己的命名空间。
 *
 * 命名空间与键值清单（改这些一定要同步改 NVS_FMT_VERSION_CURRENT 并写迁移）
 * ------------------------------------------------------------------------------
 *   命名空间：CONFIG_APP_EDK60_NVS_NAMESPACE（默认 "edk60"，Kconfig 可改）
 *
 *   键名          类型            含义                              默认值 / 说明
 *   ------------  --------------  --------------------------------  --------------------------
 *   "dev_name"    string          设备名                            "EDK60-DEMO"
 *   "fmt_ver"     u32             数据格式版本（迁移用）             1（旧）→ 2（当前）
 *   "tx_interval" i32             上报间隔（秒，演示 i32）            60
 *   "led_level"   u32             LED 亮度 0..255（演示 u32）         128（v2 新增字段）
 *   "boot_cnt"    u32             启动（复位）次数，每次启动 +1       0（首次写入后为 1）
 *   "stat_blob"   blob（32 字节）  运行统计快照，见 edk60_stat_blob_t  无（首次写入时生成）
 *
 * 关于 blob：edk60_stat_blob_t 的**二进制布局就是它的存储格式**，一旦发布就不能
 * 随意改字段——所以下面用 _Static_assert 把 sizeof 钉死：改结构体就会编译失败，
 * 逼你提升 fmt_ver 并写迁移逻辑，而不是让老数据被静默解释错。
 *
 * 关键设计取舍
 * ------------
 *   * 命名空间名与键名都是**短名**：NVS 键名上限是 NVS_KEY_NAME_MAX_SIZE=16
 *     （含结尾 '\0'，见 nvs.h），所以键名必须 <= 15 个字符；本 demo 的键名全部合规。
 *   * boot_cnt 每启动一次写一次 flash：这是需求本身要的（自检信息），
 *     NVS 内部有磨损均衡，但也不要把它做成「高频计数」（例如每秒 +1）。
 *
 * !! 破坏性操作（擦除）的守则 —— 本 demo 严格遵守 !!
 *   1) 默认**不**做任何擦除；nvs_flash_init() 失败时只打印提示，让用户自己去改 Kconfig；
 *   2) 只有 CONFIG_APP_EDK60_NVS_AUTO_ERASE_ON_FULL=y 时才会在「分区不可用」时擦除，
 *      且擦除前打印**将被擦除的完整范围**（整个 "nvs" 分区 = 所有命名空间）；
 *   3) 「演示擦除」只在 CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO=y 时可用，
 *      作用是 nvs_erase_all(本命名空间 handle)，**只影响本 demo 的命名空间**，
 *      且需要「超长按请求 + 5 秒内再短按确认」两道运行期动作；
 *   4) 本 demo **不提供**「一键清空整个 NVS」这种动作。
 *
 * 硬件与板级事实
 * --------------
 *   * BOOT 按键 = IO9，低电平按下（4.7k 上拉），同时是 C6 的 strapping 脚：
 *     复位瞬间按住会进下载模式，所以「上电时按住键」不是 bug。
 *   * LED3(TX) 与 BOOT 键共用 IO9：edk60_led_init() 把 IO9 配成输出，
 *     而 edk60_button_init_isr() 又把它改回输入（带上拉）。
 *     因此本 demo 在本文件里**只把 LED2(RX/IO8) 当心跳灯**，不用 LED3 —— 详见
 *     led_heartbeat() 上的注释。顺序上也必须先 led_init 再 button init。
 *   * 硬件依据以 docs/SCH_EDK60_VA.pdf（EDK60_VA V1.0）与
 *     components/edk60_board/include/edk60_board.h 为准，不凭记忆猜 GPIO。
 *
 * 交互说明（都打印在串口上，方便对照）
 * -----------------------------------
 *   短按（< 800 ms）           : 读取并打印当前所有键值
 *   长按（>= 1500 ms）          : 写入一组新的演示数据（打印旧值 → 新值）
 *   双击（两次短按，间隔 < 400 ms）: 打印 NVS 分区统计 + 本命名空间键清单
 *   超长按（>= 3000 ms）        : 请求擦除本命名空间（默认关闭；需再短按确认）
 *   每 30 秒                    : 心跳，打印运行时长与 boot_cnt
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "edk60_board.h"
#include "edk60_demo.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "demo_nvs";

/* ===========================================================================
 * 键名与版本
 * =========================================================================== */

#define NVS_NS              CONFIG_APP_EDK60_NVS_NAMESPACE

#define KEY_DEV_NAME        "dev_name"
#define KEY_FMT_VER         "fmt_ver"
#define KEY_TX_INTERVAL     "tx_interval"
#define KEY_LED_LEVEL       "led_level"
#define KEY_BOOT_COUNT      "boot_cnt"
#define KEY_STAT_BLOB       "stat_blob"

/* 数据格式版本：1 = 老格式（无 led_level），2 = 当前格式
 *
 * !! 类型必须显式定型为 (uint32_t)，不要写成裸的 `2u` !!
 * 本工具链（riscv32-esp-elf / newlib，ESP32-C6）的 ABI 上：
 *     uint32_t == unsigned long      （不是 unsigned int！）
 *     <inttypes.h> 的 PRIu32 展开为 "lu"（期望 unsigned long）
 * 裸字面量 `2u` 的类型是 unsigned int，与 PRIu32 不匹配，在
 * -Wall -Werror -Wextra 下会直接报 -Werror=format= 而编译失败。
 * 把常量本身定型成 uint32_t，就与 PRIu32 精确匹配，且保留 PRIu32 的
 * 可移植性（换到 uint32_t == unsigned int 的目标上依然正确）。
 * 同理：凡是 `%u` 的实参，本文件统一显式转成 (unsigned)（== unsigned int）。 */
#define NVS_FMT_VERSION_LEGACY  ((uint32_t)1)
#define NVS_FMT_VERSION_CURRENT ((uint32_t)2)

#define DEV_NAME_DEFAULT    "EDK60-DEMO"
#define TX_INTERVAL_DEFAULT (60)
#define LED_LEVEL_DEFAULT   ((uint32_t)128)

/*
 * !! IDF bool 配置项的取值陷阱（踩过一次，写下来）!!
 *   bool 配置取值为 n 时，IDF **不会**在 sdkconfig.h 里定义 CONFIG_xxx
 *   （sdkconfig 里只有一行注释 "# CONFIG_xxx is not set"）。
 *   因此 `CONFIG_XXX ? "a" : "b"` 这种**运行期三元表达式**会编译失败
 *   （'CONFIG_XXX' undeclared）；这类开关只能走 #if / #ifdef 条件编译。
 *   下面两个危险开关默认就是 n，所以必须条件编译取字符串——
 *   这同时保证了「危险操作默认关闭」的语义不被破坏。
 */
#if defined(CONFIG_APP_EDK60_NVS_AUTO_ERASE_ON_FULL)
#define NVS_CFG_AUTO_ERASE_STR "开(危险：初始化失败时会擦除整个 nvs 分区)"
#else
#define NVS_CFG_AUTO_ERASE_STR "关(默认：初始化失败时只提示、不擦除)"
#endif

#if defined(CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO)
#define NVS_CFG_ERASE_DEMO_STR "开(危险：超长按+确认后擦除本命名空间)"
#else
#define NVS_CFG_ERASE_DEMO_STR "关(默认：超长按只打印提示、不擦除)"
#endif

/* 任务与交互参数（毫秒） */
#define SHORT_PRESS_MS      800    /* < 此值算短按 */
#define WRITE_HOLD_MS       1500   /* >= 此值算长按 → 写入新演示数据 */
#define ERASE_HOLD_MS       3000   /* >= 此值算超长按 → 发出擦除请求 */
#define DOUBLE_CLICK_MS     400    /* 两次短按间隔 < 此值算双击 */
#define ERASE_CONFIRM_MS    5000   /* 擦除确认窗口 */
#define HEARTBEAT_MS        30000

/* ===========================================================================
 * blob：运行统计快照
 * ===========================================================================
 * 字段顺序与对齐即存储格式；sizeof 必须是 32（见下面的 _Static_assert）。
 * checksum 只对**前面 28 字节**做累加校验（把校验字段自己排除在外）。
 * 说明：真实产品请用 CRC32；这里为了让 demo 零额外依赖，用一个简单的
 *       「乘 31 累加」校验，够用来演示「blob 里带自校验」这个实践。
 */
#define STAT_BLOB_MAGIC     0x364B4445u   /* 'E','D','K','6'（小端字节序即 "EDK6"） */
#define STAT_BLOB_ADC_COUNT (3)           /* 采样 A0..A2 三路 */

typedef struct {
    uint32_t magic;                        /* == STAT_BLOB_MAGIC 才认为内容可信 */
    uint16_t fmt_ver;                      /* blob 自己的格式版本 */
    uint16_t reserved;                     /* 对齐占位 */
    uint32_t boot_cnt;                     /* 写入时的启动次数 */
    uint32_t uptime_s;                     /* 写入时的运行时长（秒） */
    int32_t  adc_mv[STAT_BLOB_ADC_COUNT];  /* A0..A2 校准后毫伏；-1 = 本次取不到 */
    uint32_t checksum;                     /* 前 28 字节的校验值 */
} edk60_stat_blob_t;

_Static_assert(sizeof(edk60_stat_blob_t) == 32,
               "stat_blob 的二进制布局变了！请提升 NVS_FMT_VERSION_CURRENT 并补迁移逻辑");

/* ===========================================================================
 * 模块状态
 * =========================================================================== */

static nvs_handle_t s_nvs;
static bool         s_nvs_ready;
static bool         s_nvs_write_ok = true;  /* false = 发现数据版本更新，只读保护 */
static uint32_t     s_boot_cnt;            /* 本次启动的计数（已写入 NVS） */
static int64_t      s_start_us;

static QueueHandle_t s_btn_queue;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_adc_cali[STAT_BLOB_ADC_COUNT];
static bool                      s_adc_ready;

typedef enum {
    BTN_EV_PRESSED = 0,
    BTN_EV_RELEASED,
} btn_ev_kind_t;

typedef struct {
    btn_ev_kind_t kind;
    uint32_t      hold_ms;
    uint32_t      tick_ms;      /* 回调时刻的 FreeRTOS tick 折算毫秒 */
} btn_msg_t;

/* ===========================================================================
 * 小工具
 * =========================================================================== */

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t uptime_s(void)
{
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000);
}

static const char *type_name(nvs_type_t t)
{
    switch (t) {
    case NVS_TYPE_U8:     return "u8";
    case NVS_TYPE_I8:     return "i8";
    case NVS_TYPE_U16:    return "u16";
    case NVS_TYPE_I16:    return "i16";
    case NVS_TYPE_U32:    return "u32";
    case NVS_TYPE_I32:    return "i32";
    case NVS_TYPE_U64:    return "u64";
    case NVS_TYPE_I64:    return "i64";
    case NVS_TYPE_FLOAT:  return "float";
    case NVS_TYPE_DOUBLE: return "double";
    case NVS_TYPE_STR:    return "string";
    case NVS_TYPE_BLOB:   return "blob";
    case NVS_TYPE_ANY:    return "any";
    default:              return "unknown";
    }
}

static uint32_t blob_checksum(const edk60_stat_blob_t *b)
{
    const uint8_t *p = (const uint8_t *)b;
    uint32_t sum = 0;

    /* 只累加 checksum 字段之前的字节 */
    for (size_t i = 0; i + sizeof(uint32_t) <= sizeof(*b); i++) {
        sum = sum * 31u + p[i];
    }
    return sum ? sum : 1u;      /* 避免 0 与「未写入」混淆 */
}

/* ===========================================================================
 * ADC（给 blob 提供有意义的模拟量）
 * =========================================================================== */

static esp_err_t adc_demo_ensure(void)
{
    if (s_adc_ready) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = EDK60_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 初始化失败: %s（blob 里的 ADC 字段将写 -1）", esp_err_to_name(err));
        return err;
    }

    const adc_channel_t chans[STAT_BLOB_ADC_COUNT] = {
        EDK60_ADC_CH_A0, EDK60_ADC_CH_A1, EDK60_ADC_CH_A2,
    };
    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,      /* 约 0 ~ 3.3V */
        .bitwidth = ADC_BITWIDTH_12,   /* 0 ~ 4095 */
    };

    for (int i = 0; i < STAT_BLOB_ADC_COUNT; i++) {
        err = adc_oneshot_config_channel(s_adc, chans[i], &ccfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ADC 通道 %d 配置失败: %s", (int)chans[i], esp_err_to_name(err));
            continue;
        }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cf = {
            .unit_id = EDK60_ADC_UNIT,
            .chan = chans[i],
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&cf, &s_adc_cali[i]) != ESP_OK) {
            s_adc_cali[i] = NULL;
        }
#else
        s_adc_cali[i] = NULL;
#endif
    }

    s_adc_ready = true;
    return ESP_OK;
}

/* 返回校准后的毫伏值；取不到返回 -1（blob 里就是 -1，绝不填 0 冒充有效值） */
static int32_t adc_read_mv(int idx)
{
    if (!s_adc_ready || idx < 0 || idx >= STAT_BLOB_ADC_COUNT) {
        return -1;
    }

    const adc_channel_t chans[STAT_BLOB_ADC_COUNT] = {
        EDK60_ADC_CH_A0, EDK60_ADC_CH_A1, EDK60_ADC_CH_A2,
    };

    int32_t sum = 0;
    int valid = 0;
    int raw = 0;
    for (int i = 0; i < 8; i++) {           /* 8 次平均，压一压噪声 */
        if (adc_oneshot_read(s_adc, chans[idx], &raw) == ESP_OK) {
            sum += raw;
            valid++;
        }
    }
    if (valid == 0) {
        return -1;
    }
    int avg = (int)(sum / valid);

    if (s_adc_cali[idx] == NULL) {
        return -1;                          /* 没校准就不编造毫伏值 */
    }
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_adc_cali[idx], avg, &mv) != ESP_OK) {
        return -1;
    }
    return (int32_t)mv;
}

/* ===========================================================================
 * NVS 初始化（带「不静默擦除」策略）
 * =========================================================================== */

static esp_err_t nvs_open_ns(void)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(\"%s\") 失败: %s", NVS_NS, esp_err_to_name(err));
    }
    return err;
}

/*
 * 初始化 NVS。
 * 返回 ESP_OK 表示可用；其它值表示不可用（调用方只打印、不做任何写入）。
 *
 * !! 破坏性操作边界 !!
 *   本函数**默认只读**：遇到 NO_FREE_PAGES / NEW_VERSION_FOUND 这类必须擦除才能恢复
 *   的情况，默认只打印「怎么处理」，不碰 flash。只有用户显式打开
 *   CONFIG_APP_EDK60_NVS_AUTO_ERASE_ON_FULL=y（默认 n）才会执行 nvs_flash_erase()，
 *   而那是**整个 "nvs" 分区**（所有命名空间，含 Wi-Fi 凭据）都会被清掉的操作。
 */
static esp_err_t nvs_init_with_policy(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        printf("[NVS] nvs_flash_init() 成功\n");
        return ESP_OK;
    }

    bool must_erase = (err == ESP_ERR_NVS_NO_FREE_PAGES ||
                       err == ESP_ERR_NVS_NEW_VERSION_FOUND);

    if (!must_erase) {
        printf("[NVS] nvs_flash_init() 失败: %s（%d）—— 非「需擦除」类错误，"
               "本 demo 不做任何写入。\n", esp_err_to_name(err), (int)err);
        return err;
    }

    printf("\n!!!!!!!!!!!!!!!!!! 需要人工决策的 NVS 异常 !!!!!!!!!!!!!!!!!!\n");
    printf("[NVS] nvs_flash_init() = %s（%d）\n", esp_err_to_name(err), (int)err);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        printf("      含义：NVS 分区里没有空页了（写满，或分区被截断过）。\n");
    } else {
        printf("      含义：NVS 里的数据是新版格式，当前固件不识别（NEW_VERSION_FOUND）。\n");
    }
    printf("      这两种情况都只能靠「擦除分区」恢复。\n");
    printf("  >>> 破坏性操作范围说明（请先读完再决定）<<<\n");
    printf("      被擦对象：分区表中名为 \"" NVS_DEFAULT_PART_NAME "\" 的**整个** NVS 分区\n");
    printf("      会丢失  ：该分区下**所有命名空间**的全部键值，包括但不限于：\n");
    printf("                Wi-Fi 配网凭据、其它组件/demo 的校准值与配置、\n");
    printf("                本 demo 的 \"%s\" 命名空间全部键值（dev_name/fmt_ver/.../boot_cnt）\n", NVS_NS);
    printf("      不可恢复：是。擦除后无法找回。\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

#if CONFIG_APP_EDK60_NVS_AUTO_ERASE_ON_FULL
    /* 用户已在 Kconfig 里显式授权（默认 n，属于「显式开关」） */
    printf("[NVS] 检测到 CONFIG_APP_EDK60_NVS_AUTO_ERASE_ON_FULL=y → 按开关授权执行擦除。\n");
    printf("[NVS] 即将擦除：整个 \"" NVS_DEFAULT_PART_NAME "\" 分区（不可恢复）。\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));     /* 先把日志刷出去，再动 flash */

    err = nvs_flash_erase();
    if (err != ESP_OK) {
        printf("[NVS] nvs_flash_erase() 失败: %s（%d）\n", esp_err_to_name(err), (int)err);
        return err;
    }
    printf("[NVS] 擦除完成，重新初始化 NVS...\n");

    err = nvs_flash_init();
    if (err != ESP_OK) {
        printf("[NVS] 擦除后 nvs_flash_init() 仍然失败: %s（%d）\n",
               esp_err_to_name(err), (int)err);
        return err;
    }
    printf("[NVS] 擦除后初始化成功（分区已是空的）。\n");
    return ESP_OK;
#else
    printf("[NVS] CONFIG_APP_EDK60_NVS_AUTO_ERASE_ON_FULL=n（默认）→ **不自动擦除**，"
           "demo 到此停止，不做任何写入。\n");
    printf("[NVS] 恢复方式（二选一）：\n");
    printf("      1) menuconfig → \"EDK60 Demo 参数（低功耗/存储）\"\n");
    printf("         → [*] 危险：NVS 无法初始化时自动擦除整个 nvs 分区；\n");
    printf("         重新编译烧写后跑本 demo，它会打印擦除范围并执行。\n");
    printf("      2) 或手工执行：idf.py -p <PORT> erase-flash\n");
    printf("         （注意：erase-flash 会把固件一起擦掉，需要重新烧写。）\n");
    printf("[NVS] 风险提示：以上两种都会清空整个 NVS 分区，"
           "不要在有现场数据/已配网凭据的板子上随手执行。\n");
    return err;
#endif
}

/* ===========================================================================
 * 数据格式版本与迁移
 * =========================================================================== */

/*
 * 确保数据结构版本正确。
 * 返回值：
 *   ESP_OK                 —— 版本一致，或已完成迁移；
 *   ESP_ERR_NOT_FOUND      —— 首次运行且默认值写入失败；
 *   ESP_ERR_INVALID_VERSION—— 数据来自更新版本的固件，调用方应转入「只读保护」；
 *   其它                   —— NVS 读写错误。
 */
static esp_err_t nvs_ensure_schema(void)
{
    uint32_t ver = 0;
    esp_err_t err = nvs_get_u32(s_nvs, KEY_FMT_VER, &ver);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("[NVS/迁移] 首次运行（没有 \"%s\"）→ 写入全部默认值并把格式版本置为 %"
               PRIu32 "\n", KEY_FMT_VER, NVS_FMT_VERSION_CURRENT);

        esp_err_t e1 = nvs_set_str(s_nvs, KEY_DEV_NAME, DEV_NAME_DEFAULT);
        esp_err_t e2 = nvs_set_i32(s_nvs, KEY_TX_INTERVAL, (int32_t)TX_INTERVAL_DEFAULT);
        esp_err_t e3 = nvs_set_u32(s_nvs, KEY_LED_LEVEL, (uint32_t)LED_LEVEL_DEFAULT);
        esp_err_t e4 = nvs_set_u32(s_nvs, KEY_FMT_VER, (uint32_t)NVS_FMT_VERSION_CURRENT);
        if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK) {
            printf("[NVS/迁移] 写入默认值失败: %s / %s / %s / %s\n",
                   esp_err_to_name(e1), esp_err_to_name(e2),
                   esp_err_to_name(e3), esp_err_to_name(e4));
            return e1 != ESP_OK ? e1 : (e2 != ESP_OK ? e2 : (e3 != ESP_OK ? e3 : e4));
        }
        err = nvs_commit(s_nvs);
        if (err != ESP_OK) {
            printf("[NVS/迁移] 提交默认值失败: %s\n", esp_err_to_name(err));
            return err;
        }
        printf("[NVS/迁移] 默认值已写入：%s=\"%s\" %s=%d %s=%u %s=%" PRIu32 "\n",
               KEY_DEV_NAME, DEV_NAME_DEFAULT, KEY_TX_INTERVAL, TX_INTERVAL_DEFAULT,
               KEY_LED_LEVEL, (unsigned)LED_LEVEL_DEFAULT,
               KEY_FMT_VER, (uint32_t)NVS_FMT_VERSION_CURRENT);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        printf("[NVS/迁移] 读取 \"%s\" 失败: %s（%d）\n", KEY_FMT_VER, esp_err_to_name(err), (int)err);
        return err;
    }

    if (ver == NVS_FMT_VERSION_CURRENT) {
        printf("[NVS/迁移] \"%s\" = %" PRIu32 "（当前版本）→ 无需迁移\n",
               KEY_FMT_VER, ver);
        return ESP_OK;
    }

    if (ver > NVS_FMT_VERSION_CURRENT) {
        printf("[NVS/迁移] !! \"%s\" = %" PRIu32 " 比本固件认识的 %" PRIu32
               " 还新 → 数据来自更新版本的固件。\n", KEY_FMT_VER, ver, NVS_FMT_VERSION_CURRENT);
        printf("           保护性处理：本 demo 之后**只读不写**，"
               "避免用老逻辑把新数据改坏。\n");
        return ESP_ERR_INVALID_VERSION;
    }

    /* ver < 当前：执行迁移 */
    printf("[NVS/迁移] 发现旧格式 \"%s\" = %" PRIu32 "，目标版本 = %" PRIu32 "，开始迁移\n",
           KEY_FMT_VER, ver, NVS_FMT_VERSION_CURRENT);

    if (ver == NVS_FMT_VERSION_LEGACY) {
        /* v1 → v2 的迁移：v2 新增了 led_level，老数据没有这个键，补默认值 */
        uint32_t level = 0;
        err = nvs_get_u32(s_nvs, KEY_LED_LEVEL, &level);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            printf("            v1 → v2：新增键 \"%s\"，写入默认值 %u\n",
                   KEY_LED_LEVEL, (unsigned)LED_LEVEL_DEFAULT);
            err = nvs_set_u32(s_nvs, KEY_LED_LEVEL, (uint32_t)LED_LEVEL_DEFAULT);
            if (err != ESP_OK) {
                printf("            写入 \"%s\" 失败: %s\n", KEY_LED_LEVEL, esp_err_to_name(err));
                return err;
            }
        } else if (err != ESP_OK) {
            printf("            读取 \"%s\" 失败: %s\n", KEY_LED_LEVEL, esp_err_to_name(err));
            return err;
        } else {
            printf("            v1 → v2：\"%s\" 已存在（%" PRIu32 "），保持不变\n",
                   KEY_LED_LEVEL, level);
        }
    } else {
        printf("            未知的旧版本 %" PRIu32 "：本 demo 只做已知迁移，"
               "这里直接对齐版本号（真实产品请显式处理每一级）\n", ver);
    }

    err = nvs_set_u32(s_nvs, KEY_FMT_VER, (uint32_t)NVS_FMT_VERSION_CURRENT);
    if (err != ESP_OK) {
        printf("[NVS/迁移] 写入 \"%s\" 失败: %s\n", KEY_FMT_VER, esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        printf("[NVS/迁移] 迁移提交失败: %s\n", esp_err_to_name(err));
        return err;
    }
    printf("[NVS/迁移] 迁移完成，\"%s\" 已更新为 %" PRIu32 "\n",
           KEY_FMT_VER, (uint32_t)NVS_FMT_VERSION_CURRENT);
    return ESP_OK;
}

/* ===========================================================================
 * 启动计数（板级自检信息）
 * =========================================================================== */

static esp_err_t nvs_bump_boot_count(void)
{
    if (!s_nvs_write_ok) {
        printf("[NVS/自检] 处于「只读保护」状态（数据版本比固件新）→ 不做启动计数写入\n");
        return ESP_ERR_INVALID_VERSION;
    }

    uint32_t cnt = 0;
    esp_err_t err = nvs_get_u32(s_nvs, KEY_BOOT_COUNT, &cnt);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        cnt = 0;                    /* 第一次：下面 +1 就变成 1 */
        printf("[NVS/自检] 首次运行，\"%s\" 不存在 → 从 0 开始计数\n", KEY_BOOT_COUNT);
    } else if (err != ESP_OK) {
        printf("[NVS/自检] 读取 \"%s\" 失败: %s\n", KEY_BOOT_COUNT, esp_err_to_name(err));
        return err;
    }

    cnt++;
    err = nvs_set_u32(s_nvs, KEY_BOOT_COUNT, cnt);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        printf("[NVS/自检] 写入 \"%s\" 失败: %s\n", KEY_BOOT_COUNT, esp_err_to_name(err));
        return err;
    }

    s_boot_cnt = cnt;
    printf("========================================\n");
    printf("[NVS/自检] >>> 本机第 %" PRIu32 " 次启动 <<<  "
           "（持久化在 NVS 的 \"%s\" 键里）\n", cnt, KEY_BOOT_COUNT);
    printf("           取整块板的寿命视角：这是第 %" PRIu32 " 次上电/复位运行。\n", cnt);
    printf("           复位原因见上方启动横幅（edk60_print_reset_reason）。\n");
    printf("========================================\n");
    return ESP_OK;
}

/* ===========================================================================
 * 读取并打印全部键值
 * =========================================================================== */

static void print_stat_blob_value(const edk60_stat_blob_t *b, bool valid)
{
    if (!valid) {
        printf("    stat_blob      : <不存在或读取失败>\n");
        return;
    }
    printf("    stat_blob      : magic=0x%08" PRIX32 " fmt_ver=%u boot_cnt=%" PRIu32
           " uptime=%" PRIu32 "s adc_mv=[%d,%d,%d] checksum=%s\n",
           b->magic, (unsigned)b->fmt_ver, b->boot_cnt, b->uptime_s,
           (int)b->adc_mv[0], (int)b->adc_mv[1], (int)b->adc_mv[2],
           (blob_checksum(b) == b->checksum) ? "OK" : "不匹配(数据可能损坏)");
}

static void nvs_read_and_print_all(void)
{
    if (!s_nvs_ready) {
        printf("[NVS] demo 未就绪（NVS 不可用），无法读取\n");
        return;
    }

    printf("\n--------------- 当前 NVS 内容（命名空间 \"%s\"）---------------\n", NVS_NS);

    /* 1) string */
    char strbuf[64] = { 0 };
    size_t slen = sizeof(strbuf);
    esp_err_t err = nvs_get_str(s_nvs, KEY_DEV_NAME, strbuf, &slen);
    if (err == ESP_OK) {
        printf("    %-14s : string  \"%s\"（%u 字节含结尾）\n",
               KEY_DEV_NAME, strbuf, (unsigned)slen);
    } else {
        printf("    %-14s : string  <读不到: %s>\n", KEY_DEV_NAME, esp_err_to_name(err));
    }

    /* 2) u32 */
    uint32_t u = 0;
    err = nvs_get_u32(s_nvs, KEY_FMT_VER, &u);
    if (err == ESP_OK) {
        printf("    %-14s : u32     %" PRIu32 "\n", KEY_FMT_VER, u);
    } else {
        printf("    %-14s : u32     <读不到: %s>\n", KEY_FMT_VER, esp_err_to_name(err));
    }

    /* 3) i32 */
    int32_t i = 0;
    err = nvs_get_i32(s_nvs, KEY_TX_INTERVAL, &i);
    if (err == ESP_OK) {
        printf("    %-14s : i32     %" PRId32 "（秒）\n", KEY_TX_INTERVAL, i);
    } else {
        printf("    %-14s : i32     <读不到: %s>\n", KEY_TX_INTERVAL, esp_err_to_name(err));
    }

    /* 4) u32（v2 新增字段） */
    u = 0;
    err = nvs_get_u32(s_nvs, KEY_LED_LEVEL, &u);
    if (err == ESP_OK) {
        printf("    %-14s : u32     %" PRIu32 "（0..255）\n", KEY_LED_LEVEL, u);
    } else {
        printf("    %-14s : u32     <读不到: %s>\n", KEY_LED_LEVEL, esp_err_to_name(err));
    }

    /* 5) u32 启动计数 */
    u = 0;
    err = nvs_get_u32(s_nvs, KEY_BOOT_COUNT, &u);
    if (err == ESP_OK) {
        printf("    %-14s : u32     %" PRIu32 "（次）\n", KEY_BOOT_COUNT, u);
    } else {
        printf("    %-14s : u32     <读不到: %s>\n", KEY_BOOT_COUNT, esp_err_to_name(err));
    }

    /* 6) blob */
    edk60_stat_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    size_t blen = sizeof(blob);
    err = nvs_get_blob(s_nvs, KEY_STAT_BLOB, &blob, &blen);
    print_stat_blob_value(&blob, (err == ESP_OK && blen == sizeof(blob)));
    printf("----------------------------------------------------------------\n");
}

/* ===========================================================================
 * 分区统计 + 本命名空间键清单
 * =========================================================================== */

static void nvs_print_stats_and_keys(void)
{
    if (!s_nvs_ready) {
        printf("[NVS] demo 未就绪（NVS 不可用），无法统计\n");
        return;
    }

    printf("\n--------------- NVS 分区统计（nvs_get_stats）---------------\n");
    nvs_stats_t st;
    /* part_name 传 NULL = 使用默认分区 NVS_DEFAULT_PART_NAME（"nvs"） */
    esp_err_t err = nvs_get_stats(NULL, &st);
    if (err == ESP_OK) {
        printf("    total_entries     = %u    （全部条目）\n", (unsigned)st.total_entries);
        printf("    used_entries      = %u    （已用条目）\n", (unsigned)st.used_entries);
        printf("    free_entries      = %u    （空闲条目，含保留条目）\n", (unsigned)st.free_entries);
        printf("    available_entries = %u    （还能用于写数据的新条目）\n",
               (unsigned)st.available_entries);
        printf("    namespace_count   = %u    （分区里的命名空间个数，含各组件自己的）\n",
               (unsigned)st.namespace_count);
        if (st.total_entries > 0) {
            printf("    使用率            = %.1f%%\n",
                   (double)st.used_entries * 100.0 / (double)st.total_entries);
        }
    } else {
        printf("    nvs_get_stats() 失败: %s（%d）\n", esp_err_to_name(err), (int)err);
    }

    printf("\n--------------- 本命名空间 \"%s\" 的键清单（nvs_entry_find_in_handle）---\n",
           NVS_NS);
    nvs_iterator_t it = NULL;
    err = nvs_entry_find_in_handle(s_nvs, NVS_TYPE_ANY, &it);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("    （本命名空间里还没有任何键）\n");
        return;
    }
    if (err != ESP_OK) {
        printf("    枚举失败: %s（%d）\n", esp_err_to_name(err), (int)err);
        return;
    }

    int n = 0;
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) == ESP_OK) {
            n++;
            printf("    %2d. %-14s  类型 = %-6s  (namespace=%s)\n",
                   n, info.key, type_name(info.type), info.namespace_name);
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);      /* 获取到迭代器就必须释放，否则泄漏内部结构 */
    printf("    合计 %d 个键\n", n);
    printf("------------------------------------------------------------\n");
}

/* ===========================================================================
 * 写入一组新的演示数据（打印旧值 → 新值）
 * =========================================================================== */

static void nvs_write_demo_dataset(void)
{
    if (!s_nvs_ready) {
        printf("[NVS] demo 未就绪（NVS 不可用），无法写入\n");
        return;
    }
    if (!s_nvs_write_ok) {
        printf("[NVS] 处于「只读保护」状态（数据版本比固件新），拒绝写入\n");
        return;
    }

    printf("\n--------------- 写入一组新的演示数据 ----------------\n");

    /* --- 1) string：dev_name --- */
    char old_str[96] = { 0 };
    size_t slen = sizeof(old_str);
    esp_err_t err = nvs_get_str(s_nvs, KEY_DEV_NAME, old_str, &slen);
    if (err != ESP_OK) {
        snprintf(old_str, sizeof(old_str), "%s", "<不存在或读不出>");
    }
    char new_str[64];
    snprintf(new_str, sizeof(new_str), "%s#%" PRIu32, DEV_NAME_DEFAULT, s_boot_cnt);
    err = nvs_set_str(s_nvs, KEY_DEV_NAME, new_str);
    printf("    %-14s string : \"%s\"  ->  \"%s\"%s\n",
           KEY_DEV_NAME, old_str, new_str, (err == ESP_OK) ? "" : "  [写入失败]");

    /* --- 2) i32：tx_interval（用随机数制造可见变化；真实产品里这是配置项） --- */
    int32_t old_i = 0;
    (void)nvs_get_i32(s_nvs, KEY_TX_INTERVAL, &old_i);
    int32_t new_i = 30 + (int32_t)(esp_random() % 5u) * 15;   /* 30/45/60/75/90 */
    err = nvs_set_i32(s_nvs, KEY_TX_INTERVAL, new_i);
    printf("    %-14s i32    : %" PRId32 "  ->  %" PRId32 "%s\n",
           KEY_TX_INTERVAL, old_i, new_i, (err == ESP_OK) ? "" : "  [写入失败]");

    /* --- 3) u32：led_level --- */
    uint32_t old_u = 0;
    (void)nvs_get_u32(s_nvs, KEY_LED_LEVEL, &old_u);
    uint32_t new_u = 32u + (esp_random() % 224u);             /* 32..255 */
    err = nvs_set_u32(s_nvs, KEY_LED_LEVEL, new_u);
    printf("    %-14s u32    : %" PRIu32 "  ->  %" PRIu32 "%s\n",
           KEY_LED_LEVEL, old_u, new_u, (err == ESP_OK) ? "" : "  [写入失败]");

    /* --- 4) blob：stat_blob（含 uptime / 启动次数 / ADC 采样 / 自校验） --- */
    edk60_stat_blob_t old_blob;
    size_t oblen = sizeof(old_blob);
    bool old_ok = (nvs_get_blob(s_nvs, KEY_STAT_BLOB, &old_blob, &oblen) == ESP_OK &&
                   oblen == sizeof(old_blob));

    edk60_stat_blob_t nb;
    memset(&nb, 0, sizeof(nb));
    nb.magic = STAT_BLOB_MAGIC;
    nb.fmt_ver = (uint16_t)NVS_FMT_VERSION_CURRENT;
    nb.boot_cnt = s_boot_cnt;
    nb.uptime_s = uptime_s();
    for (int i = 0; i < STAT_BLOB_ADC_COUNT; i++) {
        nb.adc_mv[i] = adc_read_mv(i);
    }
    nb.checksum = blob_checksum(&nb);

    printf("    %-14s blob   : ", KEY_STAT_BLOB);
    if (old_ok) {
        printf("boot_cnt=%" PRIu32 " uptime=%" PRIu32 "s adc=[%d,%d,%d]",
               old_blob.boot_cnt, old_blob.uptime_s,
               (int)old_blob.adc_mv[0], (int)old_blob.adc_mv[1], (int)old_blob.adc_mv[2]);
    } else {
        printf("<不存在>");
    }
    printf("\n                              ->  boot_cnt=%" PRIu32 " uptime=%" PRIu32
           "s adc=[%d,%d,%d]（%u 字节，checksum 已写入）\n",
           nb.boot_cnt, nb.uptime_s, (int)nb.adc_mv[0], (int)nb.adc_mv[1], (int)nb.adc_mv[2],
           (unsigned)sizeof(nb));

    err = nvs_set_blob(s_nvs, KEY_STAT_BLOB, &nb, sizeof(nb));
    if (err != ESP_OK) {
        printf("    %-14s blob   : [写入失败: %s]\n", KEY_STAT_BLOB, esp_err_to_name(err));
    }

    /* --- 5) 统一 commit：nvs_set_* 之后必须 commit 才真正落盘 --- */
    err = nvs_commit(s_nvs);
    if (err == ESP_OK) {
        printf("    nvs_commit()   : 成功，以上 4 类数据已落盘\n");
    } else {
        printf("    nvs_commit()   : 失败: %s（数据可能未落盘）\n", esp_err_to_name(err));
    }
    printf("    提示：ADC 采样的是 A0..A2（悬空脚会读到浮空噪声，属正常；勿超 3.3V）。\n");
    printf("----------------------------------------------------\n");
}

/* ===========================================================================
 * 擦除演示（破坏性操作，三重保险）
 * =========================================================================== */

#if CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO
/*
 * 在 5 秒确认窗口内等待「再短按一次」作为确认。
 * 返回 true = 用户确认；false = 超时或收到别的动作。
 */
static bool nvs_erase_confirm_window(void)
{
    printf("[NVS/擦除] 确认窗口已开启：请在 %d 秒内**再短按一次 BOOT 键**表示确认。\n",
           ERASE_CONFIRM_MS / 1000);
    printf("          窗口内任何其它按键动作都不算确认；不做任何动作 = 超时取消。\n");
    fflush(stdout);

    const int steps = ERASE_CONFIRM_MS / 100;
    for (int i = 0; i < steps; i++) {
        btn_msg_t m;
        if (xQueueReceive(s_btn_queue, &m, pdMS_TO_TICKS(100)) == pdPASS) {
            if (m.kind == BTN_EV_RELEASED && m.hold_ms < WRITE_HOLD_MS) {
                printf("[NVS/擦除] 收到短按确认。\n");
                return true;
            }
            printf("[NVS/擦除] 收到非确认的按键动作（按住 %" PRIu32 " ms），忽略，继续等待。\n",
                   m.hold_ms);
        }
    }
    printf("[NVS/擦除] 确认窗口超时 → **已取消，未做任何擦除**。\n");
    return false;
}
#endif /* CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO */

/*
 * 擦除本 demo 的命名空间。
 *
 * !! 破坏性操作 —— 作用范围与前置条件 !!
 *   作用范围：只影响 handle 对应的**本命名空间**（CONFIG_APP_EDK60_NVS_NAMESPACE，
 *             默认 "edk60"）里的全部键值 —— dev_name / fmt_ver / tx_interval /
 *             led_level / boot_cnt / stat_blob 全部清掉，boot_cnt 会从 0 重新开始。
 *             **不影响**其它命名空间（Wi-Fi 凭据、其它组件的配置都不动），
 *             因为 nvs_erase_all() 是作用在 namespace handle 上的。
 *   不可恢复：是。擦除后本命名空间的键值无法找回。
 *   三重保险（本 demo 的守则）：
 *     1) 编译期：CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO=y（默认 n），关掉就彻底没有这条路；
 *     2) 运行期动作 1：BOOT 键超长按 >= 3000 ms 才会「请求」擦除；
 *     3) 运行期动作 2：5 秒内再短按一次作为确认。
 *
 * 返回值：true = 真的执行了擦除。
 */
static bool nvs_erase_namespace_demo(void)
{
#if !CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO
    printf("\n[NVS/擦除] 检测到超长按（擦除请求），"
           "但 CONFIG_APP_EDK60_NVS_ENABLE_ERASE_DEMO=n（默认）\n");
    printf("          → **不执行**任何擦除，本命名空间的数据全部保持原样。\n");
    printf("          确实要演示擦除时，请在 menuconfig 里打开：\n");
    printf("          \"EDK60 Demo 参数（低功耗/存储）\" → "
           "[*] 危险：允许演示擦除本 demo 的命名空间\n");
    return false;
#else
    if (!s_nvs_ready) {
        printf("[NVS/擦除] demo 未就绪（NVS 不可用），无法执行\n");
        return false;
    }
    if (!s_nvs_write_ok) {
        printf("[NVS/擦除] 处于「只读保护」状态（数据版本比固件新），拒绝擦除\n");
        return false;
    }

    printf("\n!!!!!!!!!!!!!!!!!! 破坏性操作请求 !!!!!!!!!!!!!!!!!!\n");
    printf("[NVS/擦除] 触发源：BOOT 键超长按（>= %d ms）\n", ERASE_HOLD_MS);
    printf("  >>> 将被擦除的范围（请确认这是你想做的）<<<\n");
    printf("      分区    ：\"" NVS_DEFAULT_PART_NAME "\"（默认 NVS 分区）\n");
    printf("      命名空间：\"%s\"（**只有这一个**）\n", NVS_NS);
    printf("      会被清空的键：\n");
    printf("        %s / %s / %s / %s / %s / %s\n",
           KEY_DEV_NAME, KEY_FMT_VER, KEY_TX_INTERVAL,
           KEY_LED_LEVEL, KEY_BOOT_COUNT, KEY_STAT_BLOB);
    printf("      不受影响：该分区下**其它**命名空间（Wi-Fi 凭据、其它组件配置等）\n");
    printf("      可恢复性：**不可恢复**\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    if (!nvs_erase_confirm_window()) {
        return false;
    }

    printf("[NVS/擦除] 已确认 → 执行 nvs_erase_all(命名空间 \"%s\") ...\n", NVS_NS);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));    /* 日志先刷出，再动 flash */

    esp_err_t err = nvs_erase_all(s_nvs);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        printf("[NVS/擦除] 失败: %s（%d）—— 数据可能仍存在\n", esp_err_to_name(err), (int)err);
        return false;
    }
    printf("[NVS/擦除] 完成：命名空间 \"%s\" 已被清空。\n", NVS_NS);

    /* 命名空间被 erase_all 删掉后，句柄对新数据的解释不再可靠：
     * 关闭并重新打开，保证后续读写从干净状态开始。 */
    nvs_close(s_nvs);
    if (nvs_open_ns() != ESP_OK) {
        s_nvs_ready = false;
        printf("[NVS/擦除] 重新打开命名空间失败，后续操作已停止\n");
        return true;
    }
    printf("[NVS/擦除] 命名空间已重新打开；下次启动时 "
           "\"%s\" 会从 0 重新计数。\n", KEY_BOOT_COUNT);
    return true;
#endif
}

/* ===========================================================================
 * 按键交互
 * =========================================================================== */

/*
 * ISR 回调（在板级的按键任务上下文执行，可以 printf，但这里刻意什么都不打印、
 * 也不做 flash 操作）：只把事件塞进队列，所有耗时工作交给 nvs_demo_task。
 */
static void on_button_event(edk60_button_event_t event, uint32_t hold_ms, void *user_ctx)
{
    (void)user_ctx;

    btn_msg_t msg = {
        .kind = (event == EDK60_BUTTON_EVENT_PRESSED) ? BTN_EV_PRESSED : BTN_EV_RELEASED,
        .hold_ms = hold_ms,
        .tick_ms = now_ms(),
    };

    if (s_btn_queue != NULL) {
        /* 回调不能阻塞：队列满就丢掉这个事件（按键类交互丢一个事件不致命） */
        (void)xQueueSend(s_btn_queue, &msg, 0);
    }
}

/* 心跳：只闪 LED2(RX/IO8)。
 * 不用 LED3(TX/IO9) 的原因：IO9 与 BOOT 键共用一个脚，
 * edk60_button_init_isr() 已把 IO9 配成带上拉的输入 —— 此时再往 IO9 写电平
 * 既点不亮灯（pin 是输入），在按键按下时还会形成输出对地的灌电流风险。
 * 所以本 demo 的心跳灯固定用 IO8（下面用 LED 逻辑编号 0，不是 GPIO 号）。 */
#define NVS_DEMO_LED_RX_INDEX  0     /* 0 = LED_RX(IO8)，见 edk60_board.h 的 LED 索引约定 */

static void led_heartbeat(void)
{
    edk60_led_set(NVS_DEMO_LED_RX_INDEX, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    edk60_led_set(NVS_DEMO_LED_RX_INDEX, false);
}

/* ===========================================================================
 * 主任务
 * =========================================================================== */

static void nvs_demo_task(void *arg)
{
    (void)arg;

    s_start_us = esp_timer_get_time();

#if !CONFIG_APP_EDK60_STARTUP_BANNER
    /* 横幅关闭时 app_main 不会打印复位原因，这里补一次；
     * 横幅默认开着，那时由 app_main 打印，避免重复。 */
    edk60_print_reset_reason();
#endif

    /* ---------- 1. NVS 初始化（可疑情况默认不擦除） ---------- */
    printf("\n================ NVS 初始化（分区 \"" NVS_DEFAULT_PART_NAME
           "\"，命名空间 \"%s\"）================\n", NVS_NS);
    if (nvs_init_with_policy() != ESP_OK) {
        printf("[NVS] 初始化未成功 → 本 demo 不做任何读写，仅保留心跳。\n");
        printf("      修好之后重新运行即可；这不是程序崩溃，是设计上的「显式失败」。\n");
    } else if (nvs_open_ns() != ESP_OK) {
        printf("[NVS] 打开命名空间失败 → 本 demo 不做任何读写。\n");
    } else {
        s_nvs_ready = true;
        printf("[NVS] 就绪：命名空间 \"%s\"，句柄已打开（READWRITE）\n", NVS_NS);

        /* ---------- 2. 数据格式版本 / 迁移 ---------- */
        esp_err_t schema_err = nvs_ensure_schema();
        if (schema_err == ESP_ERR_INVALID_VERSION) {
            s_nvs_write_ok = false;     /* 只读保护：见 nvs_ensure_schema() 的注释 */
        }

        /* ---------- 3. 启动计数 ---------- */
        nvs_bump_boot_count();

        /* ---------- 4. 首次打印一遍全量内容，便于对照后面的「旧值 → 新值」 ---------- */
        nvs_read_and_print_all();
        nvs_print_stats_and_keys();
    }
    printf("========================================================\n");

    /* ---------- 5. LE D 与按键：顺序不能反（见文件头 IO9 复用说明） ---------- */
    esp_err_t err = edk60_led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED 初始化失败: %s（心跳灯可能不亮，不影响 NVS 功能）",
                 esp_err_to_name(err));
    }
    edk60_led_set_all(false);

    /* 给 blob 里的 ADC 字段准备采集能力（失败不影响主流程，字段写 -1） */
    (void)adc_demo_ensure();

    s_btn_queue = xQueueCreate(8, sizeof(btn_msg_t));
    if (s_btn_queue == NULL) {
        ESP_LOGE(TAG, "按键事件队列创建失败（内存不足），按键交互不可用");
    } else {
        err = edk60_button_init_isr(on_button_event, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "按键中断注册失败: %s，按键交互不可用", esp_err_to_name(err));
        }
    }

    printf("\n[交互] BOOT 键（IO%d）动作定义：\n", EDK60_PIN_BUTTON_BOOT);
    printf("        短按（< %d ms）               → 读取并打印当前所有键值\n", SHORT_PRESS_MS);
    printf("        长按（>= %d ms）              → 写入一组新的演示数据\n", WRITE_HOLD_MS);
    printf("        双击（两次短按，间隔 < %d ms）→ 打印 NVS 统计 + 键清单\n", DOUBLE_CLICK_MS);
    printf("        超长按（>= %d ms）            → 请求擦除本命名空间（默认关闭，需再短按确认）\n",
           ERASE_HOLD_MS);
    printf("        每 %d 秒                      → 心跳：运行时长 + boot_cnt\n", HEARTBEAT_MS / 1000);
    printf("        注意：IO%d 是 strapping 脚，复位瞬间按住会进下载模式，"
           "「上电时按住」不是 bug。\n", EDK60_PIN_BUTTON_BOOT);

    /* ---------- 6. 事件循环 ---------- */
    bool     pending_single = false;      /* 有一次短按在等「双击窗口」结束 */
    uint32_t pending_at_ms = 0;
    uint32_t last_hb_ms = now_ms();

    while (1) {
        btn_msg_t msg;
        bool got = (s_btn_queue != NULL &&
                    xQueueReceive(s_btn_queue, &msg, pdMS_TO_TICKS(100)) == pdPASS);

        if (got && msg.kind == BTN_EV_RELEASED) {
            if (msg.hold_ms >= ERASE_HOLD_MS) {
                pending_single = false;
                (void)nvs_erase_namespace_demo();
            } else if (msg.hold_ms >= WRITE_HOLD_MS) {
                pending_single = false;
                nvs_write_demo_dataset();
            } else if (msg.hold_ms < SHORT_PRESS_MS) {
                /* 短按：可能是单击，也可能是双击的第二下 */
                if (pending_single && (msg.tick_ms - pending_at_ms) <= DOUBLE_CLICK_MS) {
                    pending_single = false;
                    printf("[交互] 识别为双击（两次短按间隔 %" PRIu32 " ms）\n",
                           msg.tick_ms - pending_at_ms);
                    nvs_print_stats_and_keys();
                } else {
                    pending_single = true;
                    pending_at_ms = msg.tick_ms;
                    printf("[交互] 识别为短按（按住 %" PRIu32 " ms）"
                           "→ 等 %d ms 看是否有第二下（双击）\n",
                           msg.hold_ms, DOUBLE_CLICK_MS);
                }
            } else {
                /* SHORT_PRESS_MS .. WRITE_HOLD_MS 之间的「中等按」：明确说明如何处理 */
                printf("[交互] 按住 %" PRIu32 " ms 落在 %d..%d ms 之间，"
                       "既不算短按也不算长按 → 忽略（定义见上面的动作表）\n",
                       msg.hold_ms, SHORT_PRESS_MS, WRITE_HOLD_MS);
            }
        }

        /* 双击窗口结束 → 落实「单击」这个动作 */
        if (pending_single && (now_ms() - pending_at_ms) > DOUBLE_CLICK_MS) {
            pending_single = false;
            nvs_read_and_print_all();
        }

        /* 心跳 */
        if ((now_ms() - last_hb_ms) >= HEARTBEAT_MS) {
            last_hb_ms = now_ms();
            printf("[心跳] 运行 %" PRIu32 " 秒 | 空闲堆 %u 字节 | NVS 启动计数 = %" PRIu32 "\n",
                   uptime_s(),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                   s_boot_cnt);
            if (s_nvs_ready) {
                led_heartbeat();
            }
        }
    }
}

void demo_nvs_rw_start(void)
{
    ESP_LOGI(TAG, "=== nvs_rw：NVS 读写 / 统计 / 键遍历 / 受控擦除 ===");
    /* 危险开关的状态用条件编译取字符串：bool 为 n 时 CONFIG_xxx 未定义，
     * 不能写成运行期三元表达式（原因见 NVS_CFG_AUTO_ERASE_STR 上方的注释）。 */
    ESP_LOGI(TAG, "命名空间=%s | 自动擦除开关=%s | 擦除演示开关=%s",
             NVS_NS, NVS_CFG_AUTO_ERASE_STR, NVS_CFG_ERASE_DEMO_STR);

    /* 任务栈 4096（要求 >= 4096）：本任务会做 NVS 读写、printf 与 ADC 采样 */
    BaseType_t ok = xTaskCreate(nvs_demo_task, "nvs_demo", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败（内存不足），nvs_rw demo 无法启动");
    }
}
