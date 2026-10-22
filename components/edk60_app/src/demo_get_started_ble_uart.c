/*
 * EDK60 Demo 2.3 —— ble_uart（BLE 串口透传，Nordic UART Service 兼容）
 *
 * 目的
 * ----
 * 用 ESP32-C6 的 BLE（NimBLE 协议栈）做一个「蓝牙串口」：
 *   - 对外提供一个 NUS（Nordic UART Service）兼容的 GATT 服务，
 *     手机端用 nRF Connect / nRF Toolbox / 自研 App 连接后即可双向收发；
 *   - 手机写入的数据回显到串口（ASCII + HEX 双视图），并原样 notify 回手机；
 *   - 每 5 秒通过 TX 特征发一条心跳，用来确认 notify 通路一直活着；
 *   - 连接 / 断开 / MTU 变化都会在串口打印，便于观察链路参数协商。
 *
 * 硬件依据
 * --------
 * 板卡 EDK60（EDK60_VA V1.0），主控 ESP32-C6-MINI-1-N4（4MB Flash，无 PSRAM）。
 *   * ESP32-C6 只支持 BLE，没有经典蓝牙（BR/EDR），因此广播 Flags 里声明
 *     "BR/EDR Not Supported"；NimBLE 也只初始化 ESP_BT_MODE_BLE。
 *   * 指示 LED（原理图丝印 -> GPIO，低电平点亮；宏定义在
 *     components/edk60_board/include/edk60_board.h，本文件不写死 GPIO 编号）：
 *       LED2 "RX" = EDK60_PIN_LED_RX = IO8 —— 每收到一包手机写入的数据闪一下；
 *       LED3 "TX" = EDK60_PIN_LED_TX = IO9 —— 每发出一包 notify 闪一下。
 *     实际 GPIO 编号只在日志里通过 edk60_led_pin() 打印出来。
 *   * IO8 / IO9 都是 C6 的 strapping 脚（IO9 同时是 BOOT 按键），
 *     这里只把 LED 当输出用、短促闪烁，不改变上电电平设计。
 *
 * 观察点
 * ------
 *   1) 串口：NimBLE 初始化流程 -> 本机 BLE 地址（PUBLIC/RANDOM）-> GATT 表
 *      （NUS 服务 + RX/TX 特征）-> 广播参数；
 *   2) 手机 nRF Connect 扫描到 "EDK60-BLE-UART"（可用 menuconfig 改名），
 *      连接后能看到服务 6E400001-B5A3-F393-E0A9-E50E24DCCA9E，其下两个特征：
 *        6E400002-...  写（WRITE | WRITE_NO_RSP）  手机 -> 设备（记作 RX）
 *        6E400003-...  通知 + 读（NOTIFY | READ）  设备 -> 手机（记作 TX）
 *   3) 订阅 TX 特征的 CCCD 后：串口每 5 秒打印一条心跳发送记录，
 *      手机端周期性收到 "[EDK60 BLE UART] heartbeat #N"；
 *   4) 往 RX 特征写 "hello"：串口打印 ASCII 与 HEX 两个视图、LED_RX 闪一下；
 *      紧接着手机端在 TX 上收到同样的 "hello"、LED_TX 闪一下；
 *   5) 手机断开：串口打印断开原因、对端地址与地址类型，随后自动重新广播。
 *
 * 实现要点（也是「为什么这么写」）
 * ------------------------------
 *   * 初始化顺序：nvs_flash_init() -> nimble_port_init() -> 配置 ble_hs_cfg
 *     -> 注册 GATT 表 -> 设置设备名 -> ble_store_config_init()
 *     -> nimble_port_freertos_init() 起主机任务 -> 主机 sync 回调里开始广播。
 *     IDF v6.1 的 nimble_port_init() 内部已经完成 controller init/enable 与 HCI
 *     绑定，**不再需要** 旧版的 esp_nimble_hci_and_controller_init()。
 *   * NimBLE 的 GAP 事件回调与 GATT 读写回调都运行在 NimBLE 主机任务上下文里，
 *     因此回调内只做「少量内存拷贝 + 入队」，不做阻塞操作、不做长 printf；
 *     真正的打印、LED 闪烁、notify 都交给独立的 FreeRTOS 任务完成。
 *   * 本 demo 不做配对/绑定（Just Works），连上即可收发，方便反复复现；
 *     需要加密/绑定的场景请参考 IDF 自带示例 examples/bluetooth/nimble/bleprph。
 *   * 本文件不含任何删除/擦除类破坏性操作：NVS 初始化失败只报错，
 *     不会调用 nvs_flash_erase()。
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/* --- NimBLE（IDF v6.1 的 bt 组件，ESP32-C6 只跑 NimBLE 主机 + 内置控制器） --- */
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* NVS 存储后端（配对信息用）；NimBLE 只在 ble_store_config.h 之外的 .c 里声明，
 * IDF 自带示例（NimBLE_Beacon / bleprph）也是这样前置声明的。 */
void ble_store_config_init(void);

static const char *TAG = "demo_ble_uart";

/* ===========================================================================
 * 编译期参数
 * =========================================================================== */

/** GAP 设备名（menuconfig: APP_EDK60_BLE_DEVICE_NAME，默认 "EDK60-BLE-UART"）。
 *  同时写进 GAP 服务的 Device Name 特征（0x2A00）与主广播包的 Complete Local Name。 */
#define EDK60_BLE_UART_NAME         CONFIG_APP_EDK60_BLE_DEVICE_NAME

/** 心跳发送周期（毫秒） */
#define EDK60_BLE_UART_HB_PERIOD_MS (5000)

/** 单包最大长度：MTU 上限 517 -> ATT 负载 514，这里取 256 足够一般透传。 */
#define EDK60_BLE_UART_PKT_MAX      (256)

/** 接收队列深度（满了就丢包并计数，避免回调里阻塞） */
#define EDK60_BLE_UART_QUEUE_LEN    (8)

/** 串口回显时最多 dump 多少字节（避免一包数据刷满串口） */
#define EDK60_BLE_UART_DUMP_MAX     (96)

/** LED 索引：与 edk60_board.h 的约定一致 —— 0 = 丝印 RX(IO8)，1 = 丝印 TX(IO9) */
#define EDK60_BLE_UART_LED_RX       (0)
#define EDK60_BLE_UART_LED_TX       (1)

/** LED 闪烁时长（毫秒） */
#define EDK60_BLE_UART_LED_PULSE_MS (15)

/** 任务栈（要求 >= 4096 字节） */
#define EDK60_BLE_UART_TASK_STACK   (4096)
#define EDK60_BLE_UART_TASK_PRIO    (5)

/* ===========================================================================
 * NUS（Nordic UART Service）UUID
 * ---------------------------------------------------------------------------
 * 规范形式（人可读）：
 *   服务    6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX 特征 6E400002-B5A3-F393-E0A9-E50E24DCCA9E（手机写、设备收）
 *   TX 特征 6E400003-B5A3-F393-E0A9-E50E24DCCA9E（设备发、手机收）
 *
 * NimBLE 的 ble_uuid128_t.value[16] 是「小端」存放，也就是把上面 canonical
 * 形式的 16 个字节倒过来写，所以 BLE_UUID128_INIT 的参数顺序是
 *   ...9E, CA, DC, 24, 0E, E5, A9, E0, 93, F3, A3, B5, 01, 00, 40, 6E
 * 这与 NimBLE 自带的 services/bleuart 源码完全一致（可对照 IDF 组件源码）。
 * =========================================================================== */

static const ble_uuid128_t s_nus_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_nus_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_nus_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* ===========================================================================
 * 运行期状态
 * ---------------------------------------------------------------------------
 * 说明：这些标量分别被「NimBLE 主机任务（GAP/GATT 回调）」和「我们自己的
 * 收发任务」访问。标量读写本身是原子的，且都只表示连接状态，允许有一拍的
 * 不一致；真正的数据通路走 FreeRTOS 队列，不做裸共享。
 * =========================================================================== */

static uint8_t  s_own_addr_type;              /* 广播用的本机地址类型 */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected;             /* 是否已有中心设备连上 */
static volatile bool s_tx_subscribed;         /* 手机是否已订阅 TX 的 CCCD */
static bool     s_adv_enabled;                /* 当前是否处于广播中 */

static uint16_t s_rx_val_handle;              /* RX 特征值句柄 */
static uint16_t s_tx_val_handle;              /* TX 特征值句柄 */

/** TX 特征带 READ 属性，这里保存「最近一次发出的内容」供手机直接读取 */
static uint8_t  s_tx_last[EDK60_BLE_UART_PKT_MAX];
static uint16_t s_tx_last_len;
static SemaphoreHandle_t s_tx_mtx;            /* 保护 s_tx_last / s_tx_last_len */

/** 收包队列：GATT 回调入队，ble_uart_rx_task 出队处理 */
typedef struct {
    uint16_t len;
    uint8_t  data[EDK60_BLE_UART_PKT_MAX];
} ble_uart_pkt_t;

static QueueHandle_t s_rx_queue;
static volatile uint32_t s_rx_dropped;        /* 队列满导致的丢包数 */
static volatile uint32_t s_rx_pkt_count;      /* 累计收包数 */
static volatile uint32_t s_tx_pkt_count;      /* 累计发送（notify）数 */

/** LED 指示互斥：两个任务都会点 LED_TX，加锁避免"你灭我亮"互相打架 */
static SemaphoreHandle_t s_led_mtx;

/* 只在 ble_uart_rx_task 里使用（该任务只有一个实例），放静态区少占任务栈 */
static char s_ascii_buf[EDK60_BLE_UART_DUMP_MAX * 4 + 32];
static char s_hex_buf[EDK60_BLE_UART_DUMP_MAX * 3 + 8];

/* ===========================================================================
 * 小工具
 * =========================================================================== */

/** 把 6 字节 BLE 地址格式化为 "aa:bb:cc:dd:ee:ff"（nimble 的 val[] 是低字节在前） */
static void ble_uart_addr_str(const uint8_t *val, char *out, size_t out_sz)
{
    (void)snprintf(out, out_sz, "%02x:%02x:%02x:%02x:%02x:%02x",
                   val[5], val[4], val[3], val[2], val[1], val[0]);
}

/** 地址类型文字（BLE_ADDR_PUBLIC 等，定义见 nimble/ble.h） */
static const char *ble_uart_addr_type_str(uint8_t type)
{
    switch (type) {
    case BLE_ADDR_PUBLIC:    return "PUBLIC";
    case BLE_ADDR_RANDOM:    return "RANDOM";
    case BLE_ADDR_PUBLIC_ID: return "PUBLIC_ID";
    case BLE_ADDR_RANDOM_ID: return "RANDOM_ID";
    default:                 return "UNKNOWN";
    }
}

/** 浅闪一下指定 LED（加锁，避免多任务互相覆盖） */
static void ble_uart_led_pulse(int led_index)
{
    if (s_led_mtx != NULL) {
        (void)xSemaphoreTake(s_led_mtx, portMAX_DELAY);
    }
    (void)edk60_led_set(led_index, true);
    vTaskDelay(pdMS_TO_TICKS(EDK60_BLE_UART_LED_PULSE_MS));
    (void)edk60_led_set(led_index, false);
    if (s_led_mtx != NULL) {
        (void)xSemaphoreGive(s_led_mtx);
    }
}

/**
 * 把二进制数据渲染成「可读 ASCII」视图。
 * 可打印字符原样保留，换行/回车/制表/引号/反斜杠做转义，其余用 \xNN。
 */
static void ble_uart_format_ascii(const uint8_t *data, uint16_t len, char *out, size_t out_sz)
{
    size_t o = 0;

    for (uint16_t i = 0; i < len; i++) {
        if (o + 5 >= out_sz) {           /* 留给最长的 \xNN + '\0' */
            break;
        }
        uint8_t c = data[i];
        if (c == '\\') {
            out[o++] = '\\'; out[o++] = '\\';
        } else if (c == '"') {
            out[o++] = '\\'; out[o++] = '"';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c >= 0x20 && c <= 0x7e) {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, out_sz - o, "\\x%02X", (unsigned)c);
        }
    }
    out[o] = '\0';
}

/** 把二进制数据渲染成 "68 65 6C 6C 6F" 形式的 HEX 视图 */
static void ble_uart_format_hex(const uint8_t *data, uint16_t len, char *out, size_t out_sz)
{
    size_t o = 0;

    for (uint16_t i = 0; i < len; i++) {
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
 * TX 方向：向手机发 notify
 * =========================================================================== */

/**
 * 通过 TX 特征把数据 notify 给手机。
 * 返回 true 表示已交给协议栈；false 表示未连接 / 手机没订阅 TX / 发送失败。
 * 只能在任务上下文调用（内部会做 LED 闪烁与 vTaskDelay）。
 */
static bool ble_uart_notify(const uint8_t *data, uint16_t len)
{
    if (!s_connected || !s_tx_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        s_tx_val_handle == 0 || len == 0) {
        return false;
    }
    if (len > EDK60_BLE_UART_PKT_MAX) {
        len = EDK60_BLE_UART_PKT_MAX;
    }

    /* 先记下"最后一次发送的内容"，供 TX 特征的 READ 使用 */
    if (s_tx_mtx != NULL) {
        (void)xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    }
    memcpy(s_tx_last, data, len);
    s_tx_last_len = len;
    if (s_tx_mtx != NULL) {
        (void)xSemaphoreGive(s_tx_mtx);
    }

    /* ble_gatts_notify_custom 会接管这个 mbuf（无论成功失败都由它释放） */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGW(TAG, "mbuf 分配失败，notify 丢弃（%u 字节）", (unsigned)len);
        return false;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify 失败 rc=%d（conn=%u handle=%u）",
                 rc, (unsigned)s_conn_handle, (unsigned)s_tx_val_handle);
        return false;
    }

    s_tx_pkt_count++;
    ble_uart_led_pulse(EDK60_BLE_UART_LED_TX);
    return true;
}

/* ===========================================================================
 * GATT 访问回调（运行在 NimBLE 主机任务里 —— 只做拷贝 + 入队）
 * =========================================================================== */

static int ble_uart_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    (void)conn_handle;
    (void)attr_handle;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (ctxt->chr == NULL || ble_uuid_cmp(ctxt->chr->uuid, &s_nus_tx_uuid.u) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        /* TX 特征带 READ：返回"最近一次设备发出的内容" */
        uint8_t tmp[EDK60_BLE_UART_PKT_MAX];
        uint16_t n = 0;
        if (s_tx_mtx != NULL) {
            (void)xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
        }
        n = s_tx_last_len;
        if (n > 0) {
            memcpy(tmp, s_tx_last, n);
        }
        if (s_tx_mtx != NULL) {
            (void)xSemaphoreGive(s_tx_mtx);
        }

        int rc = os_mbuf_append(ctxt->om, tmp, n);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (s_rx_queue == NULL) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (ctxt->chr == NULL || ble_uuid_cmp(ctxt->chr->uuid, &s_nus_rx_uuid.u) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        /* 队列元素较大（约 260 字节），这里用一块静态缓冲先摊平再入队，
         * 避免在 NimBLE 主机任务的栈上放大对象。
         * 同一时刻只有一个 GATT 回调在跑，所以这块缓冲不会并发被写。 */
        static ble_uart_pkt_t s_stage;
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, s_stage.data,
                                     (uint16_t)sizeof(s_stage.data), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_stage.len = len;

        s_rx_pkt_count++;
        if (xQueueSend(s_rx_queue, &s_stage, 0) != pdTRUE) {
            s_rx_dropped++;
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ===========================================================================
 * GATT 表：NUS 兼容服务
 * =========================================================================== */

static const struct ble_gatt_svc_def s_nus_svcs[] = {
    {
        /* ---- 服务：6E400001-B5A3-F393-E0A9-E50E24DCCA9E ---- */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* RX：手机写、设备收 —— NUS 里叫 "RX Characteristic" */
                .uuid       = &s_nus_rx_uuid.u,
                .access_cb  = ble_uart_gatt_access,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_rx_val_handle,
            },
            {
                /* TX：设备发（notify）、手机收；带 READ 便于直接读最后一次内容。
                 * NOTIFY 属性会让 NimBLE 自动补一个 CCCD 描述符。 */
                .uuid       = &s_nus_tx_uuid.u,
                .access_cb  = ble_uart_gatt_access,
                .flags      = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
                .val_handle = &s_tx_val_handle,
            },
            {
                0, /* 本服务特征结束 */
            }
        },
    },
    {
        0, /* 服务表结束 */
    },
};

/**
 * 注册 GATT 服务表。
 * ble_svc_gap_init() / ble_svc_gatt_init() 先注册标准 GAP(0x1800) 与
 * GATT(0x1801) 服务，再追加我们自己的 NUS 服务。
 */
static int ble_uart_gatt_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(s_nus_svcs);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(s_nus_svcs);
}

/* ===========================================================================
 * 广播
 * =========================================================================== */

static int ble_uart_gap_event(struct ble_gap_event *event, void *arg);

/**
 * 开始（或重新开始）广播。
 *
 * 传统广播包只有 31 字节，塞不下「Flags + 设备名 + 128 位 UUID」，
 * 因此拆成两包：
 *   主广播包   ：Flags(3) + Complete Local Name(2+名字) + TX Power(3)
 *                —— 默认名字 "EDK60-BLE-UART" 时共 20 字节，方便任何扫描器
 *                   立刻看到设备名（被动扫描也能看到）。
 *   扫描响应包 ：Complete 128-bit NUS Service UUID(18 字节)
 *                —— 主动扫描时才能拿到，手机 App / nRF Connect 靠它识别 NUS。
 */
static void ble_uart_advertise(void)
{
    struct ble_hs_adv_fields adv;
    struct ble_hs_adv_fields rsp;
    struct ble_gap_adv_params adv_params;
    const char *name = ble_svc_gap_device_name();
    int rc;

    if (s_adv_enabled) {
        return;
    }

    /* ---------- 1) 主广播包 ---------- */
    memset(&adv, 0, sizeof(adv));
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;  /* 通用可发现 + 无经典蓝牙 */
    adv.tx_pwr_lvl_is_present = 1;
    adv.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;                   /* 由协议栈填实际值 */
    adv.name = (const uint8_t *)name;
    adv.name_len = (uint8_t)strlen(name);
    adv.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置主广播数据失败 rc=%d（设备名 %u 字节可能超出 31 字节上限）",
                 rc, (unsigned)strlen(name));
        return;
    }

    /* ---------- 2) 扫描响应包：NUS 服务 UUID ---------- */
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = &s_nus_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        /* 扫描响应设置失败不致命：主广播包仍可用，只是手机端看不到服务 UUID */
        ESP_LOGW(TAG, "设置扫描响应数据失败 rc=%d（继续广播）", rc);
    }

    /* ---------- 3) 启动广播：可连接 + 通用可发现 ---------- */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* 非定向可连接 */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* 通用可发现 */
    adv_params.itvl_min  = BLE_GAP_ADV_FAST_INTERVAL1_MIN;  /* 30ms */
    adv_params.itvl_max  = BLE_GAP_ADV_FAST_INTERVAL1_MAX;  /* 60ms */

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_uart_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败 rc=%d", rc);
        return;
    }

    s_adv_enabled = true;
    ESP_LOGI(TAG, "已开始广播：名字 \"%s\"，间隔 %u~%u (x0.625ms = 30~60ms)，"
                  "扫描响应携带 NUS 服务 UUID",
             name, (unsigned)adv_params.itvl_min, (unsigned)adv_params.itvl_max);
}

/* ===========================================================================
 * GAP 事件回调（运行在 NimBLE 主机任务里 —— 日志保持简短）
 * =========================================================================== */

static void ble_uart_print_conn_desc(const struct ble_gap_conn_desc *desc)
{
    char peer[24];
    ble_uart_addr_str(desc->peer_ota_addr.val, peer, sizeof(peer));

    ESP_LOGI(TAG, "  连接参数: handle=%u 对端=%s (%s) 间隔=%.2fms 延迟=%u 超时=%ums",
             (unsigned)desc->conn_handle, peer,
             ble_uart_addr_type_str(desc->peer_ota_addr.type),
             (double)desc->conn_itvl * 1.25,
             (unsigned)desc->conn_latency,
             (unsigned)desc->supervision_timeout * 10);
}

static int ble_uart_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            char peer[24] = "n/a";
            const char *peer_type = "UNKNOWN";
            bool have_desc = (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0);

            if (have_desc) {
                ble_uart_addr_str(desc.peer_ota_addr.val, peer, sizeof(peer));
                peer_type = ble_uart_addr_type_str(desc.peer_ota_addr.type);
            }

            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_tx_subscribed = false;

            /* 连上之后停止广播（符合 NUS 外设的常见做法），断开时再重新广播 */
            if (s_adv_enabled) {
                (void)ble_gap_adv_stop();
                s_adv_enabled = false;
            }

            ESP_LOGI(TAG, "已连接：对端地址 %s（地址类型 %s），广播已停止，等待读写",
                     peer, peer_type);
            if (have_desc) {
                ble_uart_print_conn_desc(&desc);
            }
        } else {
            ESP_LOGW(TAG, "连接建立失败 status=%d，继续广播", event->connect.status);
            s_adv_enabled = false;   /* 失败时协议栈已自动停播，这里复位标记 */
            ble_uart_advertise();
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        char peer[24];
        ble_uart_addr_str(event->disconnect.conn.peer_ota_addr.val, peer, sizeof(peer));

        ESP_LOGI(TAG, "已断开：对端地址 %s（地址类型 %s），原因 reason=%d",
                 peer, ble_uart_addr_type_str(event->disconnect.conn.peer_ota_addr.type),
                 event->disconnect.reason);

        s_connected = false;
        s_tx_subscribed = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_adv_enabled = false;       /* 断开后协议栈会自动停播，复位标记再重启 */

        ble_uart_advertise();
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "广播结束 reason=%d，重新开始广播", event->adv_complete.reason);
        s_adv_enabled = false;
        ble_uart_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        ESP_LOGI(TAG, "连接参数更新 status=%d", event->conn_update.status);
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ble_uart_print_conn_desc(&desc);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        /* 手机端发起 ATT MTU 交换后会走这里；MTU 越大单包能带的字节越多 */
        ESP_LOGI(TAG, "MTU 变化：conn_handle=%u cid=%u 新 MTU=%u 字节"
                      "（单包有效载荷 = MTU-3 = %u 字节）",
                 (unsigned)event->mtu.conn_handle,
                 (unsigned)event->mtu.channel_id,
                 (unsigned)event->mtu.value,
                 (unsigned)(event->mtu.value > 3 ? event->mtu.value - 3 : 0));
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_tx_subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "手机%s了 TX 特征的 notify 订阅（CCCD），心跳与回显%s",
                     s_tx_subscribed ? "打开" : "关闭",
                     s_tx_subscribed ? "可正常下发" : "将只回显到串口");
        } else {
            ESP_LOGI(TAG, "订阅变化：attr_handle=%u reason=%d notify %d->%d",
                     (unsigned)event->subscribe.attr_handle,
                     event->subscribe.reason,
                     event->subscribe.prev_notify, event->subscribe.cur_notify);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "加密状态变化 status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* 对端已有绑定但要求重新配对：demo 直接丢弃旧绑定并重试 */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGW(TAG, "对端重复配对，已丢弃旧绑定并重试");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

/* ===========================================================================
 * NimBLE 主机回调与主机任务
 * =========================================================================== */

static void ble_uart_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 协议栈复位，reason=%d", reason);
}

static void ble_uart_on_sync(void)
{
    uint8_t addr[6] = {0};
    char buf[24];
    int rc;

    /* 确保本机至少有一个可用 BLE 地址（prefer_random = 0：优先用公开地址） */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr 失败 rc=%d", rc);
        return;
    }

    /* 推断广播时用哪种地址 */
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto 失败 rc=%d", rc);
        return;
    }

    if (ble_hs_id_copy_addr(s_own_addr_type, addr, NULL) == 0) {
        ble_uart_addr_str(addr, buf, sizeof(buf));
        ESP_LOGI(TAG, "本机 BLE 地址 %s（类型 %s）", buf,
                 ble_uart_addr_type_str(s_own_addr_type));
    }

    ESP_LOGI(TAG, "GATT 表已就绪：服务 6E400001-...-DCCA9E（NUS）"
                  " RX@handle=%u TX@handle=%u",
             (unsigned)s_rx_val_handle, (unsigned)s_tx_val_handle);

    /* 同步完成后开始广播 */
    ble_uart_advertise();
}

static void ble_uart_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE 主机任务已启动（栈 %u 字节）",
             (unsigned)CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE);
    nimble_port_run();          /* 只在 nimble_port_stop() 后才返回 */
    vTaskDelete(NULL);
}

/* ===========================================================================
 * 业务任务
 * =========================================================================== */

/**
 * 收包任务：处理手机写入 RX 特征的数据。
 *   1) 串口打印 ASCII + HEX 双视图；
 *   2) LED_RX 闪一下；
 *   3) 原样 notify 回手机（TX 特征），LED_TX 闪一下。
 */
static void ble_uart_rx_task(void *arg)
{
    (void)arg;

    ble_uart_pkt_t *pkt = (ble_uart_pkt_t *)malloc(sizeof(ble_uart_pkt_t));
    if (pkt == NULL) {
        ESP_LOGE(TAG, "收包任务内存分配失败，任务退出");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(s_rx_queue, pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint16_t len = pkt->len;
        uint16_t dump = (len > EDK60_BLE_UART_DUMP_MAX) ? EDK60_BLE_UART_DUMP_MAX : len;

        ble_uart_format_ascii(pkt->data, dump, s_ascii_buf, sizeof(s_ascii_buf));
        ble_uart_format_hex(pkt->data, dump, s_hex_buf, sizeof(s_hex_buf));

        printf("[RX] 收到第 %u 包，共 %u 字节%s\n",
               (unsigned)s_rx_pkt_count, (unsigned)len,
               (len > dump) ? "（下面只显示前 96 字节）" : "");
        printf("     ASCII: \"%s\"\n", s_ascii_buf);
        printf("     HEX  : %s\n", s_hex_buf);
        if (s_rx_dropped > 0) {
            printf("     （队列曾满，累计丢弃 %u 包 —— 主因是串口打印跟不上）\n",
                   (unsigned)s_rx_dropped);
        }

        /* 收到一包 -> LED_RX 闪一下 */
        ble_uart_led_pulse(EDK60_BLE_UART_LED_RX);

        /* 原样回显给手机 */
        if (ble_uart_notify(pkt->data, len)) {
            printf("     -> 已通过 TX 特征 notify 回手机（累计 %u 包）\n",
                   (unsigned)s_tx_pkt_count);
        } else {
            printf("     -> 未 notify：%s\n",
                   s_connected ? "手机尚未订阅 TX 特征（请在 nRF Connect 里点通知开关）"
                               : "当前无连接");
        }
    }
}

/**
 * 心跳任务：每 5 秒通过 TX 特征 notify 一条心跳，用来确认「设备 -> 手机」通路。
 * 未连接 / 未订阅时只做低频提示，不刷屏。
 */
static void ble_uart_heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EDK60_BLE_UART_HB_PERIOD_MS));

        seq++;
        char line[64];
        int n = snprintf(line, sizeof(line), "[EDK60 BLE UART] heartbeat #%u",
                         (unsigned)seq);
        if (n < 0) {
            continue;
        }

        if (ble_uart_notify((const uint8_t *)line, (uint16_t)n)) {
            printf("[TX] 心跳 #%u 已发送（%d 字节，累计 %u 包）\n",
                   (unsigned)seq, n, (unsigned)s_tx_pkt_count);
        } else if ((seq % 6U) == 0U) {
            /* 每 30 秒提示一次，免得未连接时刷屏 */
            ESP_LOGI(TAG, "心跳 #%u：%s，notify 暂不可用", (unsigned)seq,
                     s_connected ? "手机未订阅 TX 特征" : "等待手机连接");
        }
    }
}

/* ===========================================================================
 * Demo 入口
 * =========================================================================== */

void demo_ble_uart_start(void)
{
    esp_err_t ret;
    int rc;

    ESP_LOGI(TAG, "=== ble_uart：BLE 串口透传（NUS 兼容 GATT 服务）===");

    /* ---------- 1) 指示灯 ---------- */
    ESP_ERROR_CHECK(edk60_led_init());
    ESP_ERROR_CHECK(edk60_led_set_all(false));
    s_led_mtx = xSemaphoreCreateMutex();
    if (s_led_mtx == NULL) {
        ESP_LOGE(TAG, "创建 LED 互斥锁失败（LED 指示将退化为无锁）");
    }
    ESP_LOGI(TAG, "指示灯：每收一包亮 LED_RX(IO%d)，每发一包亮 LED_TX(IO%d)（低电平点亮）",
             edk60_led_pin(EDK60_BLE_UART_LED_RX),
             edk60_led_pin(EDK60_BLE_UART_LED_TX));

    /* ---------- 2) NVS（NimBLE 的存储后端要它；失败只报错，绝不擦除） ---------- */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 需要擦除整片 NVS 才能恢复。擦除是破坏性操作，本 demo 不做，
         * 只提示用户改用 demo 4.2 nvs_rw 显式清理（或执行 idf.py erase-flash）。 */
        ESP_LOGE(TAG, "NVS 需要擦除才能初始化（ret=%d）。本 demo 不执行擦除操作，"
                      "请先烧写并运行 demo 4.2 nvs_rw 做显式清理", ret);
        return;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败 ret=%d", ret);
        return;
    }

    /* ---------- 3) 收发队列 / 互斥 ---------- */
    s_rx_queue = xQueueCreate(EDK60_BLE_UART_QUEUE_LEN, sizeof(ble_uart_pkt_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "创建收包队列失败");
        return;
    }
    s_tx_mtx = xSemaphoreCreateMutex();
    if (s_tx_mtx == NULL) {
        ESP_LOGE(TAG, "创建 TX 互斥锁失败");
        return;
    }

    /* ---------- 4) NimBLE 协议栈（controller + host 一体初始化） ---------- */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败 ret=%d", ret);
        return;
    }

    /* ---------- 5) 主机配置 ---------- */
    ble_hs_cfg.reset_cb = ble_uart_on_reset;
    ble_hs_cfg.sync_cb  = ble_uart_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 不做配对绑定：连上即可收发（Just Works）。
     * 需要 MITM / 绑定时请打开 sm_bonding / sm_mitm 并配置 sm_io_cap。 */
    ble_hs_cfg.sm_io_cap  = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 1;

    /* ---------- 6) GATT 服务（GAP + GATT + NUS） ---------- */
    rc = ble_uart_gatt_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败 rc=%d", rc);
        return;
    }

    /* ---------- 7) GAP 设备名（名字来自 menuconfig） ---------- */
    rc = ble_svc_gap_device_name_set(EDK60_BLE_UART_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置设备名失败 rc=%d", rc);
        return;
    }

    /* ---------- 8) 存储后端 + 启动主机任务 ---------- */
    ble_store_config_init();
    nimble_port_freertos_init(ble_uart_host_task);

    /* ---------- 9) 业务任务 ---------- */
    if (xTaskCreate(ble_uart_rx_task, "ble_uart_rx", EDK60_BLE_UART_TASK_STACK,
                    NULL, EDK60_BLE_UART_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建收包任务失败");
    }
    if (xTaskCreate(ble_uart_heartbeat_task, "ble_uart_hb", EDK60_BLE_UART_TASK_STACK,
                    NULL, EDK60_BLE_UART_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建心跳任务失败");
    }

    ESP_LOGI(TAG, "就绪：设备名 \"%s\"，服务 UUID 6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
             EDK60_BLE_UART_NAME);
    ESP_LOGI(TAG, "  手机侧操作：nRF Connect -> 扫描 \"%s\" -> 连接 -> "
                  "找 6E400001 服务 -> 订阅 6E400003 的通知 -> 往 6E400002 写数据",
             EDK60_BLE_UART_NAME);
}
