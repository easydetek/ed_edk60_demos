/*
 * EDK60 Demo 2.4 —— ble_scan（BLE 广播扫描 / 被动观察者）
 *
 * 目的
 * ----
 * 用 ESP32-C6 的 BLE（NimBLE 协议栈）做一个「BLE 广播嗅探器」：
 *   - 主动扫描（可切被动）持续监听空中的广播包与扫描响应包；
 *   - 解析常见 AD（Advertising Data）类型并翻译成人话：
 *       0x01 Flags、0x02/0x03 16-bit UUID、0x06/0x07 128-bit UUID、
 *       0x08/0x09 设备名、0x0A TX Power、0x16 Service Data、0xFF 厂商自定义数据；
 *   - 重点识别两类常见信标：iBeacon（厂商 ID 0x004C）与 Eddystone（服务 0xFEAA）；
 *   - 同地址去重：默认只打印「新发现」的设备，重复的包只更新 RSSI / 次数，
 *     并每 CONFIG_APP_EDK60_BLE_SUMMARY_SEC 秒打印一张按 RSSI 排序的汇总表。
 *
 * 硬件依据
 * --------
 * 板卡 EDK60（EDK60_VA V1.0），主控 ESP32-C6-MINI-1-N4（4MB Flash，无 PSRAM）。
 *   * ESP32-C6 只支持 BLE（无经典蓝牙），NimBLE 只初始化 ESP_BT_MODE_BLE；
 *     本 demo 只做观察者（Observer），不发起连接。
 *   * 本 demo 不需要任何外接器件；板上 LED 仅作「扫描任务活着」的指示：
 *       每打印一次汇总表 -> LED2 "RX"(IO8) 闪一下（GPIO 编号取自 BSP 宏，
 *       文件内不写死任何引脚号）。
 *
 * 观察点
 * ------
 *   1) 串口先打印扫描参数换算：间隔 80 x 0.625ms = 50ms、窗口 48 x 0.625ms = 30ms、
 *      占空比 60%、主动/被动扫描；
 *   2) 上电约 1 秒后开始出现 "[新设备]" 明细，包含：
 *      RSSI / 地址(类型+值) / 广播事件类型 / 是否有名字 / 广播数据长度，
 *      以及逐条 AD 解析结果（Flags、名字、TX Power、UUID、厂商数据、Service Data）；
 *   3) 把另一块 EDK60 刷成 demo 2.3 ble_uart 放在旁边：这里应能看到
 *      名字 "EDK60-BLE-UART"、AD 长度 20 字节，以及扫描响应里的 128 位 NUS UUID
 *      6E400001-B5A3-F393-E0A9-E50E24DCCA9E；
 *   4) 每 CONFIG_APP_EDK60_BLE_SUMMARY_SEC（默认 10）秒打印一次汇总表，
 *      按最近 RSSI 从强到弱排序，最多 20 行，列含「次数 / 最近 RSSI / 最近时间」；
 *   5) 打开 menuconfig 的 APP_EDK60_BLE_SCAN_VERBOSE 后，每一包广播都会打印一行。
 *
 * 实现要点（也是「为什么这么写」）
 * ------------------------------
 *   * NimBLE 的 BLE_GAP_EVENT_DISC 回调运行在 NimBLE 主机任务上下文里，
 *     回调内只做「拷贝 31 字节 + 非阻塞入队」，绝不做 printf / 解析等耗时动作；
 *     解析、去重、打印、汇总全部放在独立的 ble_scan_report_task 里完成。
 *   * 队列满了直接丢包并计数（丢弃数会在汇总行里显示）——这是刻意选择：
 *     宁可丢日志，也不让协议栈任务被串口打印拖住。
 *   * 扫描参数单位是 0.625ms（BLE 规范），Kconfig 里填的就是这个原始单位。
 *   * 本文件不含任何删除/擦除类破坏性操作。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/* --- NimBLE（IDF v6.1 的 bt 组件） --- */
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

/* NVS 存储后端（NimBLE 需要）；IDF 自带示例也是这样前置声明的 */
void ble_store_config_init(void);

static const char *TAG = "demo_ble_scan";

/* ===========================================================================
 * 编译期参数
 * =========================================================================== */

/* 主动/被动扫描：Kconfig 的 bool 在选 n 时不会生成 CONFIG_ 宏，所以用 #if 判断 */
#if CONFIG_APP_EDK60_BLE_SCAN_ACTIVE
#define EDK60_BLE_SCAN_PASSIVE      (0)
#define EDK60_BLE_SCAN_MODE_STR     "主动扫描 Active Scan（会发 SCAN_REQ，能拿到扫描响应包）"
#else
#define EDK60_BLE_SCAN_PASSIVE      (1)
#define EDK60_BLE_SCAN_MODE_STR     "被动扫描 Passive Scan（只收听广播包，省电但拿不到扫描响应）"
#endif

#if CONFIG_APP_EDK60_BLE_SCAN_VERBOSE
#define EDK60_BLE_SCAN_VERBOSE_ON   (1)
#else
#define EDK60_BLE_SCAN_VERBOSE_ON   (0)
#endif

/** 设备表容量：超出的新地址会被忽略并计数（不覆盖已发现的设备） */
#define EDK60_BLE_SCAN_MAX_DEV      (40)

/** 汇总表最多打印多少行 */
#define EDK60_BLE_SCAN_SUMMARY_ROWS (20)

/** 打印队列深度 */
#define EDK60_BLE_SCAN_QUEUE_LEN    (32)

/** 设备名最多保留多少字节 */
#define EDK60_BLE_SCAN_NAME_MAX     (32)

/** 最多记录多少个 16-bit / 128-bit UUID */
#define EDK60_BLE_SCAN_UUID16_MAX   (6)
#define EDK60_BLE_SCAN_UUID128_MAX  (2)

/** 厂商自定义数据最多记录多少字节 */
#define EDK60_BLE_SCAN_MFG_MAX      (24)

/** Service Data 最多记录多少字节 */
#define EDK60_BLE_SCAN_SVC_DATA_MAX (24)

/** 任务栈（要求 >= 4096 字节） */
#define EDK60_BLE_SCAN_TASK_STACK   (4096)
#define EDK60_BLE_SCAN_TASK_PRIO    (5)

/** 上报任务轮询周期（毫秒）：既要及时出队，又要能按时打汇总 */
#define EDK60_BLE_SCAN_POLL_MS      (200)

/** LED 索引：0 = 丝印 RX(IO8)，1 = 丝印 TX(IO9)（见 edk60_board.h） */
#define EDK60_BLE_SCAN_LED          (0)

/* ===========================================================================
 * 数据结构
 * =========================================================================== */

/** 从 GAP 回调原样搬到上报任务的原始广播包（保留原始字节，解析放到任务里做） */
typedef struct {
    uint8_t addr[6];                         /* 广播者地址（低字节在前） */
    uint8_t addr_type;                       /* BLE_ADDR_PUBLIC / RANDOM ... */
    uint8_t event_type;                      /* BLE_HCI_ADV_RPT_EVTYPE_* */
    int8_t  rssi;                            /* dBm */
    uint8_t len;                             /* 广播数据长度 */
    uint8_t data[BLE_HS_ADV_MAX_SZ];         /* 广播数据原始字节（传统广播 <= 31） */
} ble_scan_raw_t;

/** 解析后的 AD 内容 */
typedef struct {
    bool     has_flags;
    uint8_t  flags;
    bool     has_name;
    bool     name_is_complete;
    char     name[EDK60_BLE_SCAN_NAME_MAX];
    bool     has_tx_power;
    int8_t   tx_power;
    uint8_t  num_uuid16;
    uint16_t uuid16[EDK60_BLE_SCAN_UUID16_MAX];
    uint8_t  num_uuid128;
    uint8_t  uuid128[EDK60_BLE_SCAN_UUID128_MAX][16];   /* 已转成 canonical 打印顺序 */
    bool     has_mfg;
    uint16_t mfg_company;
    uint8_t  mfg_len;
    uint8_t  mfg_data[EDK60_BLE_SCAN_MFG_MAX];
    bool     has_svc_data16;
    uint16_t svc_data_uuid;
    uint8_t  svc_data_len;
    uint8_t  svc_data[EDK60_BLE_SCAN_SVC_DATA_MAX];
    uint8_t  beacon;                         /* 0=不是信标 1=iBeacon 2=Eddystone */
    uint8_t  eddystone_type;
    uint16_t major;
    uint16_t minor;
    int8_t   ref_power;                      /* 1m 处参考功率（iBeacon/Eddystone） */
} ble_scan_parsed_t;

/** 设备表条目 */
typedef struct {
    bool     used;
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint32_t count;                          /* 出现次数（收到的包数） */
    int8_t   rssi_last;
    int8_t   rssi_best;
    uint32_t first_ms;                       /* 首次见到（系统运行毫秒） */
    uint32_t last_ms;                        /* 最近见到 */
    uint8_t  adv_len;                        /* 最近一次广播数据长度 */
    uint32_t adv_hash;                       /* 广播内容指纹，用于判断"内容变了" */
    bool     has_name;
    char     name[EDK60_BLE_SCAN_NAME_MAX];
    uint8_t  beacon;                         /* 0/1(iBeacon)/2(Eddystone) */
} ble_scan_dev_t;

static ble_scan_dev_t s_dev[EDK60_BLE_SCAN_MAX_DEV];   /* 只被上报任务访问，无需加锁 */
static uint32_t       s_dev_count;                     /* 已占用条目数 */
static uint32_t       s_pkt_total;                     /* 累计处理包数 */
static uint32_t       s_dev_ignored;                   /* 设备表满而被忽略的新地址数 */

static QueueHandle_t  s_scan_queue;
static volatile uint32_t s_pkt_dropped;                /* 队列满丢弃的包数 */
static volatile uint32_t s_pkt_seen;                   /* 回调里累计看到的包数 */

static uint8_t  s_own_addr_type;
static volatile bool s_scanning;

/* 解析结果只在上报任务里使用（单实例），放静态区少占任务栈 */
static ble_scan_parsed_t s_parsed;

/* ===========================================================================
 * 小工具
 * =========================================================================== */

/** BLE 地址 -> "aa:bb:cc:dd:ee:ff"（nimble 的 val[] 低字节在前，打印时反转） */
static void ble_scan_addr_str(const uint8_t *val, char *out, size_t out_sz)
{
    (void)snprintf(out, out_sz, "%02x:%02x:%02x:%02x:%02x:%02x",
                   val[5], val[4], val[3], val[2], val[1], val[0]);
}

static const char *ble_scan_addr_type_str(uint8_t type)
{
    switch (type) {
    case BLE_ADDR_PUBLIC:    return "PUBLIC";
    case BLE_ADDR_RANDOM:    return "RANDOM";
    case BLE_ADDR_PUBLIC_ID: return "PUBLIC_ID";
    case BLE_ADDR_RANDOM_ID: return "RANDOM_ID";
    default:                 return "UNKNOWN";
    }
}

/** 广播事件类型 -> 中文说明（BLE_HCI_ADV_RPT_EVTYPE_*，见 nimble/hci_common.h） */
static const char *ble_scan_event_type_str(uint8_t type)
{
    switch (type) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:     return "ADV_IND 可连接非定向广播";
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:     return "ADV_DIRECT_IND 可连接定向广播";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:    return "ADV_SCAN_IND 可扫描非定向广播";
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND: return "ADV_NONCONN_IND 不可连接广播";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:    return "SCAN_RSP 扫描响应";
    default:                                 return "未知事件类型";
    }
}

/** 128 位 UUID：把 nimble 的小端 16 字节转成 canonical 大端再格式化 */
static void ble_scan_uuid128_str(const uint8_t *le16, char *out, size_t out_sz)
{
    uint8_t c[16];
    for (int i = 0; i < 16; i++) {
        c[i] = le16[15 - i];
    }
    (void)snprintf(out, out_sz,
                   "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                   c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
                   c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
}

/** 常见 16-bit 服务 UUID 的"人话"名字（查不到返回 NULL） */
static const char *ble_scan_uuid16_name(uint16_t uuid)
{
    switch (uuid) {
    case 0x1800: return "Generic Access (GAP)";
    case 0x1801: return "Generic Attribute (GATT)";
    case 0x1808: return "Glucose";
    case 0x1809: return "Health Thermometer 体温计";
    case 0x180A: return "Device Information (DIS)";
    case 0x180D: return "Heart Rate 心率";
    case 0x180F: return "Battery Service 电量";
    case 0x1810: return "Blood Pressure 血压";
    case 0x1812: return "Human Interface Device (HID)";
    case 0x181A: return "Environmental Sensing 环境传感";
    case 0x181C: return "User Data";
    case 0x181F: return "Continuous Glucose Monitoring";
    case 0x1826: return "Fitness Machine";
    case 0xFD6F: return "Exposure Notification (接触者追踪)";
    case 0xFEE0: return "Xiaomi 生态";
    case 0xFE9F: return "厂商自定义广播(常见于小米/生态链设备)";
    case 0xFEAA: return "Eddystone (Google Beacon)";
    case 0xFE59: return "Nordic DFU";
    case 0x2A00: return "Device Name (特征)";
    case 0x2A19: return "Battery Level (特征)";
    default:     return NULL;
    }
}

/** 广播内容指纹（FNV-1a），用来判断"同一地址但内容变了" */
static uint32_t ble_scan_adv_hash(const uint8_t *data, uint8_t len)
{
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

/* ===========================================================================
 * AD 解析（运行在上报任务里）
 * =========================================================================== */

/**
 * 解析一段广播数据里的所有 AD 结构。
 * AD 结构格式：| 长度(1B，含类型但不含本字段) | 类型(1B) | 数据(N B) | ...
 * 长度 0 表示填充结束。
 */
static void ble_scan_parse_adv(const uint8_t *data, uint8_t len, ble_scan_parsed_t *out)
{
    memset(out, 0, sizeof(*out));

    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t flen = data[i];
        if (flen == 0) {
            break;                                  /* 填充区，后面都不是真实数据 */
        }
        if ((int)i + 1 + (int)flen > (int)len) {
            break;                                  /* 结构被截断，丢弃剩余部分 */
        }

        uint8_t type  = data[i + 1];
        const uint8_t *val = &data[i + 2];
        uint8_t vlen  = (uint8_t)(flen - 1);

        switch (type) {
        case BLE_HS_ADV_TYPE_FLAGS:                 /* 0x01 */
            out->has_flags = true;
            out->flags = (vlen >= 1) ? val[0] : 0;
            break;

        case BLE_HS_ADV_TYPE_INCOMP_UUIDS16:        /* 0x02 */
        case BLE_HS_ADV_TYPE_COMP_UUIDS16:          /* 0x03 */
            for (uint8_t k = 0; k + 1 < vlen && out->num_uuid16 < EDK60_BLE_SCAN_UUID16_MAX; k += 2) {
                out->uuid16[out->num_uuid16++] = (uint16_t)(val[k] | ((uint16_t)val[k + 1] << 8));
            }
            break;

        case BLE_HS_ADV_TYPE_INCOMP_UUIDS128:       /* 0x06 */
        case BLE_HS_ADV_TYPE_COMP_UUIDS128:         /* 0x07 */
            for (uint8_t k = 0;
                 (uint8_t)(k + 16) <= vlen && out->num_uuid128 < EDK60_BLE_SCAN_UUID128_MAX;
                 k += 16) {
                memcpy(out->uuid128[out->num_uuid128], &val[k], 16);
                out->num_uuid128++;
            }
            break;

        case BLE_HS_ADV_TYPE_INCOMP_NAME:           /* 0x08 短名 */
        case BLE_HS_ADV_TYPE_COMP_NAME:             /* 0x09 完整名 */
            out->has_name = true;
            out->name_is_complete = (type == BLE_HS_ADV_TYPE_COMP_NAME);
            {
                uint8_t n = (vlen < (EDK60_BLE_SCAN_NAME_MAX - 1)) ? vlen
                                                                    : (EDK60_BLE_SCAN_NAME_MAX - 1);
                memcpy(out->name, val, n);
                out->name[n] = '\0';
            }
            break;

        case BLE_HS_ADV_TYPE_TX_PWR_LVL:            /* 0x0A */
            if (vlen >= 1) {
                out->has_tx_power = true;
                out->tx_power = (int8_t)val[0];
            }
            break;

        case BLE_HS_ADV_TYPE_SVC_DATA_UUID16:       /* 0x16 */
            if (vlen >= 2) {
                out->has_svc_data16 = true;
                out->svc_data_uuid = (uint16_t)(val[0] | ((uint16_t)val[1] << 8));
                uint8_t n = (uint8_t)(vlen - 2);
                n = (n < EDK60_BLE_SCAN_SVC_DATA_MAX) ? n : EDK60_BLE_SCAN_SVC_DATA_MAX;
                memcpy(out->svc_data, &val[2], n);
                out->svc_data_len = n;

                /* Eddystone：服务 0xFEAA，紧跟第一个字节是帧类型 */
                if (out->svc_data_uuid == 0xFEAA && n >= 1) {
                    out->beacon = 2;
                    out->eddystone_type = out->svc_data[0];
                }
            }
            break;

        case BLE_HS_ADV_TYPE_MFG_DATA:              /* 0xFF */
            if (vlen >= 2) {
                out->has_mfg = true;
                /* 公司 ID 是小端 16 位 */
                out->mfg_company = (uint16_t)(val[0] | ((uint16_t)val[1] << 8));
                uint8_t n = (uint8_t)(vlen - 2);
                n = (n < EDK60_BLE_SCAN_MFG_MAX) ? n : EDK60_BLE_SCAN_MFG_MAX;
                memcpy(out->mfg_data, &val[2], n);
                out->mfg_len = n;

                /* iBeacon：Apple 公司 ID 0x004C + 子类型 0x02 0x15 + 长度 0x15 */
                if (out->mfg_company == 0x004C && n >= 23 &&
                    out->mfg_data[0] == 0x02 && out->mfg_data[1] == 0x15) {
                    out->beacon = 1;
                    out->major = (uint16_t)(out->mfg_data[18] | ((uint16_t)out->mfg_data[19] << 8));
                    out->minor = (uint16_t)(out->mfg_data[20] | ((uint16_t)out->mfg_data[21] << 8));
                    out->ref_power = (int8_t)out->mfg_data[22];
                }
            }
            break;

        default:
            /* 其它 AD 类型本 demo 不展开，但会在"原始数据"里完整打印 */
            break;
        }

        i = (uint8_t)(i + 1 + flen);
    }
}

/** 打印 Flags 的人话解释 */
static void ble_scan_print_flags(uint8_t flags)
{
    printf("         Flags      : 0x%02X =", (unsigned)flags);
    if (flags & BLE_HS_ADV_F_DISC_LTD) {
        printf(" LE 有限可发现");
    }
    if (flags & BLE_HS_ADV_F_DISC_GEN) {
        printf(" LE 通用可发现");
    }
    if (flags & BLE_HS_ADV_F_BREDR_UNSUP) {
        printf(" 不支持经典蓝牙(BR/EDR)");
    }
    if (flags & 0x08) {
        printf(" 同时支持 LE+BR/EDR(控制器)");
    }
    if (flags & 0x10) {
        printf(" 同时支持 LE+BR/EDR(主机)");
    }
    printf("\n");
}

/** 把 HEX 字节转成 "AA BB CC" 形式 */
static void ble_scan_hex_str(const uint8_t *data, uint8_t len, char *out, size_t out_sz)
{
    size_t o = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (o + 4 >= out_sz) {
            break;
        }
        if (i > 0) {
            out[o++] = ' ';
        }
        o += (size_t)snprintf(out + o, out_sz - o, "%02X", (unsigned)data[i]);
    }
    out[o] = '\0';
}

/* ===========================================================================
 * 打印：新设备明细 / 逐包一行 / 汇总表
 * =========================================================================== */

static void ble_scan_print_detail(const ble_scan_raw_t *raw, const ble_scan_parsed_t *p)
{
    char addr[24];
    char hex[BLE_HS_ADV_MAX_SZ * 3 + 4];
    char uuid[40];

    ble_scan_addr_str(raw->addr, addr, sizeof(addr));
    ble_scan_hex_str(raw->data, raw->len, hex, sizeof(hex));

    printf("[新设备] %s  addr_type=%s(%u)  event_type=0x%02X (%s)\n",
           addr, ble_scan_addr_type_str(raw->addr_type), (unsigned)raw->addr_type,
           (unsigned)raw->event_type, ble_scan_event_type_str(raw->event_type));
    printf("         RSSI=%d dBm   广播数据长度=%u 字节   是否有名字=%s%s\n",
           (int)raw->rssi, (unsigned)raw->len,
           p->has_name ? "是" : "否",
           p->has_name ? (p->name_is_complete ? "（完整名）" : "（短名）") : "");

    if (p->has_name) {
        printf("         名字       : \"%s\"\n", p->name);
    }
    if (p->has_flags) {
        ble_scan_print_flags(p->flags);
    }
    if (p->has_tx_power) {
        printf("         TX Power   : %d dBm\n", (int)p->tx_power);
    }

    for (uint8_t k = 0; k < p->num_uuid16; k++) {
        const char *nm = ble_scan_uuid16_name(p->uuid16[k]);
        printf("         16-bit UUID: 0x%04X%s%s\n",
               (unsigned)p->uuid16[k], nm ? "  " : "", nm ? nm : "");
    }
    for (uint8_t k = 0; k < p->num_uuid128; k++) {
        ble_scan_uuid128_str(p->uuid128[k], uuid, sizeof(uuid));
        printf("         128-bit UUID: %s%s\n", uuid,
               /* 顺手把 NUS 标出来，方便和 demo 2.3 对拍 */
               (strncmp(uuid, "6E400001", 8) == 0) ? "  <- Nordic UART Service (NUS)" : "");
    }

    if (p->has_mfg) {
        ble_scan_hex_str(p->mfg_data, p->mfg_len, hex, sizeof(hex));
        printf("         厂商数据   : 公司 ID 0x%04X（小端字节 %02X %02X），"
               "共 %u 字节，原始 hex: %s\n",
               (unsigned)p->mfg_company,
               (unsigned)(p->mfg_company & 0xFF), (unsigned)(p->mfg_company >> 8),
               (unsigned)(p->mfg_len + 2), hex);
    }

    if (p->has_svc_data16) {
        ble_scan_hex_str(p->svc_data, p->svc_data_len, hex, sizeof(hex));
        const char *nm = ble_scan_uuid16_name(p->svc_data_uuid);
        printf("         ServiceData: UUID 0x%04X%s%s  数据(%u 字节): %s\n",
               (unsigned)p->svc_data_uuid, nm ? "  " : "", nm ? nm : "",
               (unsigned)p->svc_data_len, hex);
    }

    if (p->beacon == 1) {
        ble_scan_uuid128_str(&p->mfg_data[2], uuid, sizeof(uuid));
        printf("         识别为     : iBeacon（Apple 公司 ID 0x004C）\n");
        printf("                       proximity UUID = %s\n", uuid);
        printf("                       major = %u  minor = %u  1m 参考功率 = %d dBm\n",
               (unsigned)p->major, (unsigned)p->minor, (int)p->ref_power);
    } else if (p->beacon == 2) {
        printf("         识别为     : Eddystone（服务 0xFEAA），帧类型 0x%02X = %s\n",
               (unsigned)p->eddystone_type,
               (p->eddystone_type == 0x00) ? "UID" :
               (p->eddystone_type == 0x10) ? "URL" :
               (p->eddystone_type == 0x20) ? "TLM(遥测)" : "其它");
    }

    printf("         AD 原始数据: %s\n", hex);
}

/** 逐包模式：一行一台设备 */
static void ble_scan_print_verbose(const ble_scan_raw_t *raw, const ble_scan_parsed_t *p,
                                   uint32_t seq_for_dev)
{
    char addr[24];
    ble_scan_addr_str(raw->addr, addr, sizeof(addr));
    printf("[包] %s  RSSI=%-4d  len=%-2u  evt=0x%02X  name=%s  第 %u 次\n",
           addr, (int)raw->rssi, (unsigned)raw->len, (unsigned)raw->event_type,
           p->has_name ? p->name : "-", (unsigned)seq_for_dev);
}

/** 某地址的广播内容有变化时，打一行提示 */
static void ble_scan_print_changed(const ble_scan_raw_t *raw, const ble_scan_parsed_t *p)
{
    char addr[24];
    ble_scan_addr_str(raw->addr, addr, sizeof(addr));
    printf("[内容变化] %s  RSSI=%d  新长度=%u  名字=%s\n",
           addr, (int)raw->rssi, (unsigned)raw->len, p->has_name ? p->name : "-");
    if (p->has_name) {
        printf("           名字       : \"%s\"\n", p->name);
    }
    if (p->has_mfg) {
        char hex[BLE_HS_ADV_MAX_SZ * 3 + 4];
        ble_scan_hex_str(p->mfg_data, p->mfg_len, hex, sizeof(hex));
        printf("           厂商数据   : 公司 ID 0x%04X  hex: %s\n",
               (unsigned)p->mfg_company, hex);
    }
}

/**
 * 汇总表：按「最近 RSSI」从强到弱排序，最多 20 行。
 * 用插入排序（设备数 <= 40，且只每 N 秒做一次），不引入 qsort 的比较函数开销。
 */
static void ble_scan_print_summary(uint32_t uptime_ms, uint32_t print_seq)
{
    uint16_t order[EDK60_BLE_SCAN_MAX_DEV];
    uint16_t n = 0;

    for (uint16_t i = 0; i < EDK60_BLE_SCAN_MAX_DEV; i++) {
        if (s_dev[i].used) {
            order[n++] = i;
        }
    }
    for (uint16_t i = 1; i < n; i++) {
        uint16_t key = order[i];
        int16_t j = (int16_t)i - 1;
        while (j >= 0) {
            uint16_t a = order[j];
            bool worse = (s_dev[a].rssi_last < s_dev[key].rssi_last) ||
                         (s_dev[a].rssi_last == s_dev[key].rssi_last &&
                          s_dev[a].count < s_dev[key].count);
            if (!worse) {
                break;
            }
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    printf("\n===== BLE 扫描汇总 #%u | 运行 %.1f 秒 | 已发现 %u 台设备 | "
           "回调收到 %u 包，已解析 %u 包（队列丢弃 %u） =====\n",
           (unsigned)print_seq, (double)uptime_ms / 1000.0,
           (unsigned)s_dev_count, (unsigned)s_pkt_seen, (unsigned)s_pkt_total,
           (unsigned)s_pkt_dropped);
    printf("   #  %-18s %-9s %6s %7s %6s %8s %9s  %s\n",
           "ADDRESS", "TYPE", "COUNT", "RSSI", "BEST", "LAST_s", "FIRST_s", "NAME");
    printf("  --- ------------------ --------- ------ ------- ------ -------- ---------  ----\n");

    uint16_t rows = (n < EDK60_BLE_SCAN_SUMMARY_ROWS) ? n : EDK60_BLE_SCAN_SUMMARY_ROWS;
    for (uint16_t r = 0; r < rows; r++) {
        const ble_scan_dev_t *d = &s_dev[order[r]];
        char addr[24];
        ble_scan_addr_str(d->addr, addr, sizeof(addr));
        printf("  %3u  %-18s %-9s %6u %7d %6d %8.1f %9.1f  %s%s\n",
               (unsigned)(r + 1), addr,
               ble_scan_addr_type_str(d->addr_type), (unsigned)d->count,
               (int)d->rssi_last, (int)d->rssi_best,
               (double)(uptime_ms - d->last_ms) / 1000.0,
               (double)d->first_ms / 1000.0,
               d->has_name ? d->name : "-",
               (d->beacon == 1) ? " [iBeacon]" : (d->beacon == 2) ? " [Eddystone]" : "");
    }
    if (n > rows) {
        printf("  ...（共 %u 台，表格只显示 RSSI 最强的 %u 台）\n",
               (unsigned)n, (unsigned)EDK60_BLE_SCAN_SUMMARY_ROWS);
    }
    if (s_dev_ignored > 0) {
        printf("  注意：设备表已满（%u 条），另有 %u 个新地址被忽略\n",
               (unsigned)EDK60_BLE_SCAN_MAX_DEV, (unsigned)s_dev_ignored);
    }
    printf("============================================================\n\n");
}

/* ===========================================================================
 * 设备表操作（只在上报任务里调用，无需加锁）
 * =========================================================================== */

static int ble_scan_dev_find(const uint8_t *addr, uint8_t addr_type)
{
    for (uint16_t i = 0; i < EDK60_BLE_SCAN_MAX_DEV; i++) {
        if (s_dev[i].used && s_dev[i].addr_type == addr_type &&
            memcmp(s_dev[i].addr, addr, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int ble_scan_dev_alloc(void)
{
    for (uint16_t i = 0; i < EDK60_BLE_SCAN_MAX_DEV; i++) {
        if (!s_dev[i].used) {
            memset(&s_dev[i], 0, sizeof(s_dev[i]));
            s_dev[i].used = true;
            s_dev[i].rssi_best = -127;
            s_dev_count++;
            return (int)i;
        }
    }
    return -1;
}

/* ===========================================================================
 * 上报任务：解析 + 去重 + 打印 + 汇总
 * =========================================================================== */

static void ble_scan_report_task(void *arg)
{
    (void)arg;

    ble_scan_raw_t *raw = (ble_scan_raw_t *)malloc(sizeof(ble_scan_raw_t));
    if (raw == NULL) {
        ESP_LOGE(TAG, "上报任务内存分配失败，任务退出");
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_summary = xTaskGetTickCount();
    const TickType_t summary_period = pdMS_TO_TICKS((uint32_t)CONFIG_APP_EDK60_BLE_SUMMARY_SEC * 1000U);
    uint32_t summary_seq = 0;

    ESP_LOGI(TAG, "上报任务已启动：汇总周期 %d 秒，逐包打印 %s",
             CONFIG_APP_EDK60_BLE_SUMMARY_SEC, EDK60_BLE_SCAN_VERBOSE_ON ? "开" : "关");

    while (1) {
        if (xQueueReceive(s_scan_queue, raw, pdMS_TO_TICKS(EDK60_BLE_SCAN_POLL_MS)) == pdTRUE) {
            s_pkt_total++;

            ble_scan_parse_adv(raw->data, raw->len, &s_parsed);
            uint32_t hash = ble_scan_adv_hash(raw->data, raw->len);
            uint32_t uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            int idx = ble_scan_dev_find(raw->addr, raw->addr_type);
            if (idx < 0) {
                idx = ble_scan_dev_alloc();
                if (idx < 0) {
                    s_dev_ignored++;
                } else {
                    ble_scan_dev_t *d = &s_dev[idx];
                    memcpy(d->addr, raw->addr, 6);
                    d->addr_type = raw->addr_type;
                    d->count = 1;
                    d->rssi_last = raw->rssi;
                    d->rssi_best = raw->rssi;
                    d->first_ms = uptime_ms;
                    d->last_ms = uptime_ms;
                    d->adv_len = raw->len;
                    d->adv_hash = hash;
                    d->has_name = s_parsed.has_name;
                    if (s_parsed.has_name) {
                        memcpy(d->name, s_parsed.name, sizeof(d->name));
                        d->name[sizeof(d->name) - 1] = '\0';
                    }
                    d->beacon = s_parsed.beacon;

                    /* 新设备 -> 打印完整明细 */
                    ble_scan_print_detail(raw, &s_parsed);
                }
            } else {
                ble_scan_dev_t *d = &s_dev[idx];
                bool changed = (d->adv_hash != hash) || (d->adv_len != raw->len);

                d->count++;
                d->rssi_last = raw->rssi;
                if (raw->rssi > d->rssi_best) {
                    d->rssi_best = raw->rssi;
                }
                d->last_ms = uptime_ms;

                if (changed) {
                    /* 同一地址但广播内容变了（例如换了名字 / 电量变化）-> 刷新记录并提示 */
                    d->adv_hash = hash;
                    d->adv_len = raw->len;
                    d->has_name = s_parsed.has_name;
                    if (s_parsed.has_name) {
                        memcpy(d->name, s_parsed.name, sizeof(d->name));
                        d->name[sizeof(d->name) - 1] = '\0';
                    }
                    d->beacon = s_parsed.beacon;
                    if (!EDK60_BLE_SCAN_VERBOSE_ON) {
                        ble_scan_print_changed(raw, &s_parsed);
                    }
                }

                if (EDK60_BLE_SCAN_VERBOSE_ON) {
                    /* 逐包一行（内容变化时额外打一条明细） */
                    ble_scan_print_verbose(raw, &s_parsed, d->count);
                    if (changed) {
                        ble_scan_print_changed(raw, &s_parsed);
                    }
                }
            }
        }

        if ((xTaskGetTickCount() - last_summary) >= summary_period) {
            uint32_t uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            summary_seq++;
            ble_scan_print_summary(uptime_ms, summary_seq);
            last_summary = xTaskGetTickCount();

            /* 汇总表打印说明扫描任务还在跑：LED2 "RX"(IO8) 闪一下 */
            (void)edk60_led_toggle(EDK60_BLE_SCAN_LED);
            vTaskDelay(pdMS_TO_TICKS(15));
            (void)edk60_led_toggle(EDK60_BLE_SCAN_LED);
        }
    }
}

/* ===========================================================================
 * GAP 回调（运行在 NimBLE 主机任务里 —— 只做拷贝 + 非阻塞入队）
 * =========================================================================== */

static int ble_scan_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_scan_queue == NULL) {
            return 0;
        }

        ble_scan_raw_t raw;
        memset(&raw, 0, sizeof(raw));
        memcpy(raw.addr, event->disc.addr.val, 6);
        raw.addr_type = event->disc.addr.type;
        raw.event_type = event->disc.event_type;
        raw.rssi = event->disc.rssi;

        uint8_t len = event->disc.length_data;
        if (len > sizeof(raw.data)) {
            len = (uint8_t)sizeof(raw.data);
        }
        raw.len = len;
        if (len > 0 && event->disc.data != NULL) {
            memcpy(raw.data, event->disc.data, len);
        }

        s_pkt_seen++;
        if (xQueueSend(s_scan_queue, &raw, 0) != pdTRUE) {
            s_pkt_dropped++;      /* 队列满：宁可丢日志，也不阻塞协议栈任务 */
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* 本 demo 用 BLE_HS_FOREVER 扫描，正常不会走到这里；
         * 万一被上层取消，就重新开扫描，保证观察是连续的。 */
        ESP_LOGW(TAG, "扫描被结束（reason=%d），重新开始", event->disc_complete.reason);
        s_scanning = false;
        return 0;

    default:
        return 0;
    }
}

/* ===========================================================================
 * NimBLE 主机回调与主机任务
 * =========================================================================== */

static void ble_scan_start_disc(void);

static void ble_scan_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 协议栈复位，reason=%d", reason);
}

static void ble_scan_on_sync(void)
{
    uint8_t addr[6] = {0};
    char buf[24];
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr 失败 rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto 失败 rc=%d", rc);
        return;
    }
    if (ble_hs_id_copy_addr(s_own_addr_type, addr, NULL) == 0) {
        ble_scan_addr_str(addr, buf, sizeof(buf));
        ESP_LOGI(TAG, "本机 BLE 地址 %s（类型 %s，观察者角色，不对外广播）",
                 buf, ble_scan_addr_type_str(s_own_addr_type));
    }

    ble_scan_start_disc();
}

static void ble_scan_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE 主机任务已启动（栈 %u 字节）",
             (unsigned)CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE);
    nimble_port_run();          /* 只在 nimble_port_stop() 后才返回 */
    vTaskDelete(NULL);
}

/* ===========================================================================
 * 启动扫描
 * =========================================================================== */

static void ble_scan_start_disc(void)
{
    struct ble_gap_disc_params params;
    uint32_t itvl = (uint32_t)CONFIG_APP_EDK60_BLE_SCAN_INTERVAL;
    uint32_t win  = (uint32_t)CONFIG_APP_EDK60_BLE_SCAN_WINDOW;
    int rc;

    if (s_scanning) {
        return;
    }

    memset(&params, 0, sizeof(params));
    params.itvl = (uint16_t)CONFIG_APP_EDK60_BLE_SCAN_INTERVAL;   /* 单位 0.625ms */
    params.window = (uint16_t)CONFIG_APP_EDK60_BLE_SCAN_WINDOW;   /* 单位 0.625ms */
    params.filter_policy = 0;                                     /* 不做白名单过滤 */
    params.limited = 0;                                           /* 通用发现流程 */
    params.passive = EDK60_BLE_SCAN_PASSIVE;
    params.filter_duplicates = 0;   /* 关掉控制器侧去重：我们要自己统计次数与 RSSI */
    params.disable_observer_mode = 0;

    if (win > itvl) {
        ESP_LOGW(TAG, "扫描窗口(%u) > 扫描间隔(%u)，控制器可能拒绝该参数；"
                      "请把 APP_EDK60_BLE_SCAN_WINDOW 调到 <= APP_EDK60_BLE_SCAN_INTERVAL",
                 (unsigned)win, (unsigned)itvl);
    }

    ESP_LOGI(TAG, "扫描参数：间隔 %u x 0.625ms = %.1f ms，窗口 %u x 0.625ms = %.1f ms，"
                  "占空比 %.0f%%，%s",
             (unsigned)itvl, (double)itvl * 0.625, (unsigned)win, (double)win * 0.625,
             (itvl > 0) ? ((double)win * 100.0 / (double)itvl) : 0.0,
             EDK60_BLE_SCAN_MODE_STR);

    rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, ble_scan_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动扫描失败 rc=%d（ESP_ERR 之外是 NimBLE 错误码，"
                      "常见 0x%X=资源忙/已在扫描）", rc, (unsigned)BLE_HS_EBUSY);
        return;
    }
    s_scanning = true;
    ESP_LOGI(TAG, "扫描已启动：请观察 [新设备] / [内容变化] 与每 %d 秒一次的汇总表",
             CONFIG_APP_EDK60_BLE_SUMMARY_SEC);
}

/* ===========================================================================
 * Demo 入口
 * =========================================================================== */

void demo_ble_scan_start(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== ble_scan：BLE 广播扫描（观察者角色）===");

    /* ---------- 1) 指示灯（汇总表打印时闪一下，表示人还活着） ---------- */
    ESP_ERROR_CHECK(edk60_led_init());
    ESP_ERROR_CHECK(edk60_led_set_all(false));
    ESP_LOGI(TAG, "LED 指示：每打印一次汇总表，LED%u(IO%d) 闪一下",
             (unsigned)EDK60_BLE_SCAN_LED, edk60_led_pin(EDK60_BLE_SCAN_LED));

    /* ---------- 2) NVS（NimBLE 存储后端要它；失败只报错，绝不擦除） ---------- */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS 需要擦除才能初始化（ret=%d）。本 demo 不执行擦除操作，"
                      "请先运行 demo 4.2 nvs_rw 做显式清理", ret);
        return;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败 ret=%d", ret);
        return;
    }

    /* ---------- 3) 上报队列 ---------- */
    s_scan_queue = xQueueCreate(EDK60_BLE_SCAN_QUEUE_LEN, sizeof(ble_scan_raw_t));
    if (s_scan_queue == NULL) {
        ESP_LOGE(TAG, "创建上报队列失败");
        return;
    }

    /* ---------- 4) NimBLE 协议栈（controller + host 一体初始化） ---------- */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败 ret=%d", ret);
        return;
    }

    /* ---------- 5) 主机配置：只做观察者，不配对不绑定 ---------- */
    ble_hs_cfg.reset_cb = ble_scan_on_reset;
    ble_hs_cfg.sync_cb  = ble_scan_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;

    /* GAP 服务：本 demo 是纯观察者（不发广播、不建服务连接），
     * 但按 NimBLE 约定仍初始化 GAP 服务，保持与其它示例一致，
     * 也方便以后在这个文件里加"连接目标设备"的功能。 */
    ble_svc_gap_init();

    /* ---------- 6) 存储后端 + 主机任务 ---------- */
    ble_store_config_init();
    nimble_port_freertos_init(ble_scan_host_task);

    /* ---------- 7) 上报任务（解析 / 去重 / 打印 / 汇总） ---------- */
    if (xTaskCreate(ble_scan_report_task, "ble_scan_rpt", EDK60_BLE_SCAN_TASK_STACK,
                    NULL, EDK60_BLE_SCAN_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建上报任务失败");
        return;
    }

    ESP_LOGI(TAG, "就绪：设备表容量 %u 条，打印队列 %u 条",
             (unsigned)EDK60_BLE_SCAN_MAX_DEV, (unsigned)EDK60_BLE_SCAN_QUEUE_LEN);
}
