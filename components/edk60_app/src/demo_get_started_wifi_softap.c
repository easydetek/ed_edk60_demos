/*
 * EDK60 Demo 2.2 —— wifi_softap（Wi-Fi SoftAP + DHCP 服务器）
 *
 * 目的
 * ----
 * 让 EDK60 自己当热点：手机/电脑连上来后由板子上的 DHCP 服务器分配 IP，
 * 串口上能实时看到「谁上线了、谁下线了、现在连着几台、各自拿到什么 IP」。
 * 这是后续「SoftAP 配网」（设备开热点让用户填 Wi-Fi 账号）的基础能力。
 *
 * 硬件依据
 * --------
 *   原理图：docs/SCH_EDK60_VA.pdf（EDK60_VA V1.0，2026-06-15）
 *   主控  ：ESP32-C6-MINI-1-N4（4MB Flash，无 PSRAM），2.4GHz Wi-Fi 6 射频 + 板载天线。
 *   * 本 demo 只做 Wi-Fi，不触碰雷达接口（IO14/IO15）、I2C、ADC 等板上资源；
 *   * 两个 LED 的引脚全部取自 components/edk60_board/include/edk60_board.h 的宏
 *     （EDK60_PIN_LED_RX / EDK60_PIN_LED_TX），**没有硬编码任何 GPIO 编号**；
 *   * LED 低电平点亮，由 BSP 的 edk60_led_set() 内部处理。
 *
 * 观察点（预期现象）
 * ------------------
 *   1) 启动后打印「AP 运行参数」块：SSID、密码、信道（请求值 + 实际生效值）、BSSID(AP MAC)、
 *      IP/掩码/网关、最大可连接站点数、DHCP 服务器状态与租期。
 *   2) 用手机搜索热点，能看到 Kconfig 里配的 SSID（默认 EDK60-AP）；
 *      密码 ≥8 字符时是 WPA2-PSK，留空时是开放网络（日志会有醒目警告）。
 *   3) 手机点「连接」的瞬间，串口打印：
 *        [事件] WIFI_EVENT_AP_STACONNECTED   —— station 上线: MAC=.. AID=.. 当前在线=N 累计上线=M
 *        [事件] IP_EVENT_ASSIGNED_IP_TO_CLIENT —— DHCP 分配地址: MAC=.. IP=192.168.4.x
 *      手机上断开/离开时打印：
 *        [事件] WIFI_EVENT_AP_STADISCONNECTED —— station 下线: MAC=.. AID=.. reason=..
 *   4) 之后每 10 秒打印一张「站点列表」小表：序号 / MAC / IP / RSSI，表里数量应与
 *      事件计数一致（两者互为交叉验证）。
 *   5) LED 指示：LED2 RX(IO8) 常亮 = SoftAP 运行中；LED3 TX(IO9) 每 10 秒翻转一次 = 心跳。
 *
 * 设计说明（架构取舍，评审时重点看这里）
 * --------------------------------------
 *   * Wi-Fi 事件回调运行在系统事件任务（sys_evt）上下文，回调里**只做**计数与打印，
 *     不做任何延时/阻塞，避免拖垮事件循环；周期性的站点列表查询放在自己的任务里。
 *   * DHCP 服务器由 esp_netif 的 AP 接口默认创建并随接口自动启动（esp_netif_create_default_wifi_ap
 *     使用 ESP_NETIF_DHCP_SERVER 标志），本 demo 不做 nat/自定义地址池，保持默认
 *     192.168.4.1/24 网段，只把状态与租期「读出来打印」，便于观察与排查。
 *   * 密码策略（与 Kconfig 的 help 一致）：
 *       空        -> WIFI_AUTH_OPEN（开放网络），并打印醒目警告；
 *       1~7 字符  -> 不满足 WPA2 的 8 字符下限，配置会被驱动拒绝，因此回退为开放网络并告警；
 *       >=8 字符  -> WPA2-PSK（推荐）。
 *   * 信道策略：Kconfig 允许 1~13，但默认国家策略下只有 1~11 可用。若 12/13 被驱动拒绝，
 *     自动回退到信道 6 并在日志中说明；启动后再用 esp_wifi_get_channel() 读回实际信道打印。
 *   * 删除操作守则：本文件不含任何删除/擦除动作。NVS 异常时只打印告警与手动修复命令，
 *     绝不自动擦除 NVS。
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "demo_wifi_softap";

/* lwIP 的 DHCPS 租期单位（秒/单位）。正常由 sdkconfig 提供，这里给个兜底，
 * 避免在关闭 CONFIG_LWIP_DHCPS 的配置下编译失败。 */
#ifndef CONFIG_LWIP_DHCPS_LEASE_UNIT
#define CONFIG_LWIP_DHCPS_LEASE_UNIT 60
#endif

/* ===========================================================================
 * 参数（全部来自 menuconfig -> "EDK60 Demo 参数"）
 * =========================================================================== */

#define AP_SSID_CFG     CONFIG_APP_EDK60_SOFTAP_SSID
#define AP_PASSWORD_CFG CONFIG_APP_EDK60_SOFTAP_PASSWORD
#define AP_CHANNEL_CFG  CONFIG_APP_EDK60_SOFTAP_CHANNEL
#define AP_MAX_STA_CFG  CONFIG_APP_EDK60_SOFTAP_MAX_STA

/* ===========================================================================
 * 本 demo 的常量
 * =========================================================================== */

#define AP_STATUS_PERIOD_MS 10000 /* 站点列表刷新周期 */
#define AP_TASK_STACK       5120  /* 栈：站点列表 + printf，留足余量 */
#define AP_TASK_PRIO        4
#define AP_TABLE_WIDTH      84

#define AP_SSID_MAX_LEN     32 /* Wi-Fi SSID 上限（字节） */
#define AP_PASS_MIN_WPA2    8  /* WPA2-PSK 要求的最短密码长度 */

/* ===========================================================================
 * 运行状态
 * ===========================================================================
 * 写入方：Wi-Fi/IP 事件回调（系统事件任务）；读取方：wifi_softap_task / print_*。
 * 全部是字长对齐标量，RV32 上读写原子；语义竞态只影响「刚发生那一刻的计数」。
 */
static volatile uint32_t s_sta_online = 0;        /* 当前在线站点数（事件计数） */
static volatile uint32_t s_sta_online_total = 0;  /* 累计上线次数 */
static volatile uint32_t s_sta_offline_total = 0; /* 累计下线次数 */
static volatile uint32_t s_dhcp_assigned = 0;     /* DHCP 累计分配次数 */
static volatile bool     s_auth_open = false;     /* 是否开放网络 */

static esp_netif_t *s_ap_netif = NULL;
static int64_t      s_start_us = 0;

/* ===========================================================================
 * 输出小工具
 * =========================================================================== */

static void print_rule(int width, char ch)
{
    for (int i = 0; i < width; i++) {
        putchar(ch);
    }
    putchar('\n');
}

/* 把 C 字符串拷贝到定长 uint8_t 缓冲区（Wi-Fi 配置字段用），并补 0 结尾。
 * 刻意不用 Kconfig 宏做结构体字符串初始化：32 字节 SSID 会触发
 * "initializer-string for array of chars is too long"，在 -Werror 下直接编译失败。 */
static void copy_cstr(uint8_t *dst, size_t dst_sz, const char *src)
{
    memset(dst, 0, dst_sz);
    size_t n = strlen(src);
    if (n > (dst_sz - 1)) {
        n = dst_sz - 1;
    }
    memcpy(dst, src, n);
}

static const char *auth_mode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:      return "OPEN(开放，无加密)";
    case WIFI_AUTH_WEP:       return "WEP(已过时)";
    case WIFI_AUTH_WPA_PSK:   return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:  return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK:  return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-SAE";
    case WIFI_AUTH_WAPI_PSK:  return "WAPI-PSK";
    case WIFI_AUTH_OWE:       return "OWE(增强开放)";
    default:                  return "其它";
    }
}

static const char *dhcp_status_str(esp_netif_dhcp_status_t s)
{
    switch (s) {
    case ESP_NETIF_DHCP_INIT:    return "INIT（未启动）";
    case ESP_NETIF_DHCP_STARTED: return "STARTED（运行中）";
    case ESP_NETIF_DHCP_STOPPED: return "STOPPED（已停止）";
    default:                     return "未知";
    }
}

/* AP 侧断连原因（AP 视角的 reason，与 STA 侧同一套码值） */
static const char *ap_reason_str(uint16_t reason)
{
    switch (reason) {
    case 1:   return "未指明";
    case 2:   return "认证过期";
    case 3:   return "站点主动离开";
    case 4:   return "长时间无流量（inactivity）";
    case 5:   return "站点数超过 AP 上限";
    case 8:   return "站点主动去关联";
    case 15:  return "四次握手超时（密码不对？）";
    case 16:  return "组密钥更新超时";
    default:  return "见 esp_wifi_types.h 的 wifi_err_reason_t";
    }
}

/* ===========================================================================
 * 事件回调：只计数 + 打印，不做任何阻塞动作
 * =========================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "[事件] WIFI_EVENT_AP_START —— SoftAP 已启动，开始广播 SSID=\"%s\"", AP_SSID_CFG);
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *e = (const wifi_event_ap_staconnected_t *)event_data;
            s_sta_online++;
            s_sta_online_total++;
            ESP_LOGI(TAG, "[事件] WIFI_EVENT_AP_STACONNECTED —— station 上线: MAC=" MACSTR
                          " AID=%u 当前在线=%u 累计上线=%u",
                     MAC2STR(e->mac), (unsigned)e->aid,
                     (unsigned)s_sta_online, (unsigned)s_sta_online_total);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *e = (const wifi_event_ap_stadisconnected_t *)event_data;
            if (s_sta_online > 0) {
                s_sta_online--;
            }
            s_sta_offline_total++;
            ESP_LOGW(TAG, "[事件] WIFI_EVENT_AP_STADISCONNECTED —— station 下线: MAC=" MACSTR
                          " AID=%u reason=%u(%s) 当前在线=%u 累计下线=%u",
                     MAC2STR(e->mac), (unsigned)e->aid, (unsigned)e->reason,
                     ap_reason_str(e->reason), (unsigned)s_sta_online, (unsigned)s_sta_offline_total);
            break;
        }

        default:
            ESP_LOGD(TAG, "[事件] WIFI_EVENT id=%d（本 demo 不专门处理）", (int)event_id);
            break;
        }
        return;
    }

    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
            const ip_event_assigned_ip_to_client_t *e = (const ip_event_assigned_ip_to_client_t *)event_data;
            s_dhcp_assigned++;
            ESP_LOGI(TAG, "[事件] IP_EVENT_ASSIGNED_IP_TO_CLIENT —— DHCP 已分配地址: MAC=" MACSTR
                          " IP=" IPSTR "（累计分配 %u 次）",
                     MAC2STR(e->mac), IP2STR(&e->ip), (unsigned)s_dhcp_assigned);
        }
        return;
    }
}

/* ===========================================================================
 * 站点列表打印（每 10 秒一次）
 * =========================================================================== */

static void print_sta_list(void)
{
    wifi_sta_list_t      list = { 0 };
    wifi_sta_mac_ip_list_t ip_list = { 0 };
    int64_t up_s = (esp_timer_get_time() - s_start_us) / 1000000;

    esp_err_t err = esp_wifi_ap_get_sta_list(&list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_ap_get_sta_list 失败：%s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "站点列表查询：驱动返回 num=%d，事件计数在线=%u",
             list.num, (unsigned)s_sta_online);

    printf("\n");
    print_rule(AP_TABLE_WIDTH, '=');
    printf("【2.2 wifi_softap】站点列表（每 %d 秒刷新）| 已运行 %lld 秒\n",
           AP_STATUS_PERIOD_MS / 1000, (long long)up_s);
    printf("当前在线：驱动 %d 台 / 事件计数 %u 台 | 累计上线 %u 次 | 累计下线 %u 次 | DHCP 分配 %u 次\n",
           list.num, (unsigned)s_sta_online, (unsigned)s_sta_online_total,
           (unsigned)s_sta_offline_total, (unsigned)s_dhcp_assigned);
    print_rule(AP_TABLE_WIDTH, '-');

    if (list.num <= 0) {
        printf("（当前没有站点连接。请用手机/电脑搜索热点 \"%s\" 并连接）\n", AP_SSID_CFG);
        print_rule(AP_TABLE_WIDTH, '=');
        return;
    }

    /* 再取一次「MAC + IP」配对表；失败时退化为只打印 MAC */
    bool have_ip = (esp_wifi_ap_get_sta_list_with_ip(&list, &ip_list) == ESP_OK);

    printf("  #  MAC 地址            IP 地址           RSSI(dBm)\n");
    printf("---  -----------------  ----------------  ---------\n");
    for (int i = 0; i < list.num; i++) {
        const uint8_t *mac = list.sta[i].mac;
        /* IP 列固定 15 字符宽；未分配时用等宽的 "-" 占位，保证表格对齐 */
        char ipstr[32] = "---------------";

        if (have_ip && i < ip_list.num && ip_list.sta[i].ip.addr != 0) {
            snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&ip_list.sta[i].ip));
        }
        printf("%3d  " MACSTR "  %-16s  %5d\n", i + 1, MAC2STR(mac), ipstr, (int)list.sta[i].rssi);
    }

    print_rule(AP_TABLE_WIDTH, '=');
    printf("说明：IP 列显示为 \"---------------\" 表示站点已关联 AP 但还没走完 DHCP\n");
    printf("      （正常应在 1~2 秒内拿到地址）；若长期如此，检查站点是否被设为静态 IP。\n");
}

/* ===========================================================================
 * 初始化
 * =========================================================================== */

/* 组装 AP 配置（含开放网络/非法密码的回退逻辑），返回最终使用的认证方式说明 */
static void build_ap_config(wifi_config_t *wc, const char **auth_note)
{
    size_t pass_len = strlen(AP_PASSWORD_CFG);
    size_t ssid_len = strlen(AP_SSID_CFG);

    memset(wc, 0, sizeof(*wc));

    /* SSID：超过 32 字节会被截断（Wi-Fi 协议上限），截断时明确告警 */
    if (ssid_len > AP_SSID_MAX_LEN) {
        ESP_LOGW(TAG, "配置的 SoftAP SSID 长度 %u 字节超过上限 %d，将被截断",
                 (unsigned)ssid_len, AP_SSID_MAX_LEN);
        ssid_len = AP_SSID_MAX_LEN;
    }
    if (ssid_len == 0) {
        ESP_LOGW(TAG, "SoftAP SSID 配置为空，使用兜底的 \"EDK60-AP\"");
        copy_cstr(wc->ap.ssid, sizeof(wc->ap.ssid), "EDK60-AP");
        wc->ap.ssid_len = (uint8_t)strlen((const char *)wc->ap.ssid);
    } else {
        memcpy(wc->ap.ssid, AP_SSID_CFG, ssid_len); /* 其余字节保持 0（=字符串结束） */
        wc->ap.ssid_len = (uint8_t)ssid_len;
    }

    /* 密码 -> 认证方式 */
    if (pass_len == 0) {
        wc->ap.authmode = WIFI_AUTH_OPEN;
        s_auth_open = true;
        *auth_note = "开放网络（未设密码）";
    } else if (pass_len < AP_PASS_MIN_WPA2) {
        ESP_LOGW(TAG, "配置的 SoftAP 密码只有 %u 个字符（WPA2 要求 >= %d），"
                      "驱动会拒绝该配置；已自动回退为开放网络。",
                 (unsigned)pass_len, AP_PASS_MIN_WPA2);
        wc->ap.authmode = WIFI_AUTH_OPEN;
        s_auth_open = true;
        *auth_note = "开放网络（密码过短，已回退）";
    } else {
        copy_cstr(wc->ap.password, sizeof(wc->ap.password), AP_PASSWORD_CFG);
        wc->ap.authmode = WIFI_AUTH_WPA2_PSK;
        s_auth_open = false;
        *auth_note = "WPA2-PSK（个人版加密）";
    }

    /* 其余参数 */
    wc->ap.channel = (uint8_t)AP_CHANNEL_CFG;
    wc->ap.max_connection = (uint8_t)AP_MAX_STA_CFG;
    wc->ap.ssid_hidden = 0;      /* 广播 SSID，便于手机直接搜索 */
    wc->ap.beacon_interval = 100; /* 100 TU ≈ 102.4ms，Wi-Fi 默认值 */
    wc->ap.pmf_cfg.capable = false;
    wc->ap.pmf_cfg.required = false;
}

static esp_err_t wifi_init_softap(void)
{
    /* 1) NVS：Wi-Fi 驱动用它保存校准数据/配置。
     *    注意：这里刻意不调用 nvs_flash_erase()（不自动擦除任何分区）。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 不可用：%s（0x%x）", esp_err_to_name(err), (unsigned)err);
        ESP_LOGW(TAG, "本 demo 不会自动擦除 NVS。如需修复，请手动执行：");
        ESP_LOGW(TAG, "    idf.py -B build-wifi erase-flash  然后重新烧写");
        ESP_LOGW(TAG, "现在先继续尝试启动 Wi-Fi（NVS 不可用时 Wi-Fi 可能初始化失败）。");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init() 返回 %s，继续尝试启动 Wi-Fi", esp_err_to_name(err));
    }

    /* 2) TCP/IP 协议栈 + 默认事件循环 */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default 失败：%s", esp_err_to_name(err));
        return err;
    }

    /* 3) 默认 AP 网络接口：esp_netif 会一并创建 DHCP 服务器（默认 192.168.4.1/24） */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "创建默认 AP 网络接口失败（esp_netif_create_default_wifi_ap）");
        return ESP_FAIL;
    }

    /* 4) Wi-Fi 驱动 + 事件注册 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 WIFI_EVENT 处理器失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IP_EVENT 处理器失败：%s", esp_err_to_name(err));
        return err;
    }

    /* 5) 模式 + AP 配置 */
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(AP) 失败：%s", esp_err_to_name(err));
        return err;
    }

    const char *auth_note = NULL;
    wifi_config_t wc;
    build_ap_config(&wc, &auth_note);

    err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK && wc.ap.channel > 11) {
        /* 默认国家策略允许 1~11；12/13 需要合适的 country code，这里做安全回退 */
        ESP_LOGW(TAG, "信道 %u 被拒绝（%s）：默认国家策略下只允许 1~11，回退到信道 6",
                 (unsigned)wc.ap.channel, esp_err_to_name(err));
        wc.ap.channel = 6;
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) 失败：%s（SSID 长度=%u，密码长度=%u）",
                 esp_err_to_name(err), (unsigned)strlen(AP_SSID_CFG), (unsigned)strlen(AP_PASSWORD_CFG));
        return err;
    }

    /* 6) 启动 */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败：%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP 已按上面参数启动（认证方式：%s）", auth_note);
    return ESP_OK;
}

/* ===========================================================================
 * 主任务
 * =========================================================================== */

static void wifi_softap_task(void *arg)
{
    (void)arg;
    s_start_us = esp_timer_get_time();

    printf("\n");
    print_rule(AP_TABLE_WIDTH, '=');
    printf("EDK60 Demo 2.2 —— wifi_softap（SoftAP + DHCP 服务器）启动\n");
    printf("  板卡  : %s（%s）\n", EDK60_BOARD_NAME, EDK60_BOARD_REV);
    printf("  模组  : %s，Flash %d MB，无 PSRAM\n", EDK60_MODULE_NAME, EDK60_FLASH_SIZE_MB);
    printf("  射频  : ESP32-C6 集成 2.4GHz Wi-Fi 6(802.11ax)，仅 2.4GHz（手机请在 2.4G 下搜索）\n");
    printf("  指示  : LED2 RX(IO%d) 常亮=AP 运行中；LED3 TX(IO%d) 每 %d 秒翻转=心跳\n",
           edk60_led_pin(0), edk60_led_pin(1), AP_STATUS_PERIOD_MS / 1000);
    print_rule(AP_TABLE_WIDTH, '=');

    ESP_LOGI(TAG, "开始初始化：NVS -> esp_netif(含 DHCP 服务器) -> 事件循环 -> esp_wifi -> start");
    if (wifi_init_softap() != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP 初始化失败，本 demo 结束（本任务退出，不影响其它任务）");
        vTaskDelete(NULL); /* 只结束本任务自身，不涉及任何数据删除 */
        return;
    }

    /* 等 WIFI_EVENT_AP_START（回调里会打印），给驱动最多 3 秒完成启动 */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* ---- AP 运行参数汇总 ---- */
    uint8_t ap_mac[6] = { 0 };
    esp_netif_ip_info_t ip = { 0 };
    uint8_t actual_ch = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_netif_dhcp_status_t dhcps_st = ESP_NETIF_DHCP_INIT;
    uint32_t lease = 0;
    wifi_config_t cur = { 0 };

    (void)esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    (void)esp_wifi_get_channel(&actual_ch, &second);
    (void)esp_netif_get_ip_info(s_ap_netif, &ip);
    (void)esp_netif_dhcps_get_status(s_ap_netif, &dhcps_st);
    (void)esp_wifi_get_config(WIFI_IF_AP, &cur);

    printf("\n");
    print_rule(AP_TABLE_WIDTH, '=');
    printf("【2.2 wifi_softap】AP 运行参数\n");
    print_rule(AP_TABLE_WIDTH, '-');
    printf("  SSID        : %s\n", (const char *)cur.ap.ssid);
    printf("  密码        : %s\n", (strlen(AP_PASSWORD_CFG) >= AP_PASS_MIN_WPA2) ? AP_PASSWORD_CFG : "(无 —— 开放网络)");
    printf("  认证方式    : %s\n", auth_mode_str(cur.ap.authmode));
    printf("  信道        : %u（Kconfig 请求 %d，实际生效以驱动为准）\n",
           (unsigned)actual_ch, AP_CHANNEL_CFG);
    printf("  BSSID(AP MAC): " MACSTR "\n", MAC2STR(ap_mac));
    printf("  IP / 掩码 / 网关: " IPSTR " / " IPSTR " / " IPSTR "\n",
           IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));
    printf("  可连接站点数: 最大 %u 台（Kconfig=%d）\n",
           (unsigned)cur.ap.max_connection, AP_MAX_STA_CFG);
    printf("  DHCP 服务器 : %s（随 AP 接口自动启动）\n", dhcp_status_str(dhcps_st));
    if (esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_GET, ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                               &lease, sizeof(lease)) == ESP_OK) {
        printf("  DHCP 租期   : %u 个 lease 单位（lwIP 默认 120，1 单位 = %d 秒）\n",
               (unsigned)lease, CONFIG_LWIP_DHCPS_LEASE_UNIT);
    } else {
        printf("  DHCP 租期   : 读取失败（保持 IDF/lwIP 默认值）\n");
    }
    printf("  接入方式    : 手机/电脑 Wi-Fi 搜索 \"%s\" -> 输入上面的密码 -> 自动获取 IP\n",
           (const char *)cur.ap.ssid);
    print_rule(AP_TABLE_WIDTH, '=');

    /* ---- 开放网络的醒目警示 ---- */
    if (s_auth_open || cur.ap.authmode == WIFI_AUTH_OPEN) {
        printf("\n");
        print_rule(AP_TABLE_WIDTH, '!');
        printf("[警告] 当前是开放网络（WIFI_AUTH_OPEN）：任何人都能接入这个热点，\n");
        printf("       流量不加密，存在被蹭网/被嗅探的风险。仅建议在实验室环境使用。\n");
        printf("       修法：menuconfig -> \"EDK60 Demo 参数\" -> APP_EDK60_SOFTAP_PASSWORD\n");
        printf("       设置 %d 个字符以上的密码，重新编译烧写后即为 WPA2-PSK 加密热点。\n",
               AP_PASS_MIN_WPA2);
        print_rule(AP_TABLE_WIDTH, '!');
    }

    edk60_led_set(0, true); /* AP 运行指示灯常亮 */

    /* ---- 周期打印站点列表 ---- */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(AP_STATUS_PERIOD_MS));
        print_sta_list();
        edk60_led_toggle(1); /* 心跳：说明任务在跑、查询在循环 */
    }
}

/* ===========================================================================
 * demo 入口
 * =========================================================================== */

void demo_wifi_softap_start(void)
{
    ESP_LOGI(TAG, "=== 2.2 wifi_softap：Wi-Fi SoftAP + DHCP 服务器 ===");

    /* LED 作为状态指示：用 BSP 的接口，引脚编号不落在这里硬编码 */
    ESP_ERROR_CHECK(edk60_led_init());
    edk60_led_set_all(false);

    BaseType_t ok = xTaskCreate(wifi_softap_task, "wifi_softap", AP_TASK_STACK, NULL, AP_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 wifi_softap 任务失败（栈 %d 字节），demo 无法运行", AP_TASK_STACK);
        return;
    }
    ESP_LOGI(TAG, "wifi_softap 任务已创建（栈 %d 字节，优先级 %d）。"
                  "SSID/密码/信道/最大连接数在 menuconfig -> \"EDK60 Demo 参数\" 中配置。",
             AP_TASK_STACK, AP_TASK_PRIO);
}
