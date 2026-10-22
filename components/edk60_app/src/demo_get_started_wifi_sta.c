/*
 * EDK60 Demo 2.1 —— wifi_sta（Wi-Fi STA 扫描 / 连接 / 状态信息）
 *
 * 目的
 * ----
 * 用一块 EDK60 把 Wi-Fi STA 的完整链路跑通并「打印出来」，作为后续联网类
 * demo（BLE 配网、MQTT 上云等）的地基：
 *      主动扫描 -> （可选）认证关联 -> DHCP 取 IP -> 周期状态巡检 -> 断线自动重连
 *
 * 硬件依据
 * --------
 *   原理图：docs/SCH_EDK60_VA.pdf（EDK60_VA V1.0，2026-06-15）
 *   主控  ：ESP32-C6-MINI-1-N4（4MB Flash，无 PSRAM），集成 2.4GHz Wi-Fi 6
 *           （802.11ax）射频 + 板载模组天线。
 *   * 本 demo 只做 Wi-Fi，不触碰雷达接口（IO14/IO15）、I2C、ADC 等板上资源；
 *   * 用到的两个 LED 引脚全部取自 components/edk60_board/include/edk60_board.h
 *     的宏（EDK60_PIN_LED_RX / EDK60_PIN_LED_TX），**没有硬编码任何 GPIO 编号**；
 *   * LED 低电平点亮，由 BSP 的 edk60_led_set() 内部处理。
 *
 * 观察点（预期现象）
 * ------------------
 *   1) 启动后先打印一段配置摘要，紧接着是一张「扫描结果表」，7 列对齐：
 *        ID / SSID / BSSID / CH / RSSI / AUTH / WPA3
 *      最多打印 CONFIG_APP_EDK60_WIFI_SCAN_MAX_AP 条（默认 20 条）。
 *   2) 若 menuconfig 里 APP_EDK60_WIFI_SSID 为空字符串（默认值）：
 *      不发起连接，改做「只扫描模式」——每 10 秒重扫一次，并只打印信号最强的
 *      5 个 AP（观察 RSSI 随位置变化的趋势）。
 *   3) 若配置了 SSID：串口应依次看到事件流
 *        WIFI_EVENT_STA_START
 *        -> WIFI_EVENT_STA_CONNECTED（带 SSID/BSSID/信道/认证方式）
 *        -> IP_EVENT_STA_GOT_IP（带 IP / 掩码 / 网关）
 *      连上以后每 10 秒打印一行状态：IP / 掩码 / 网关 / DNS / RSSI / 信道 /
 *      信号质量 / 网卡状态 / 上网判据。
 *   4) 把路由器断电或走远：会看到
 *        WIFI_EVENT_STA_DISCONNECTED（带 reason code 与中文释义）
 *      随后是「第 N/5 次重连：X ms 后退避重试」的日志；路由器恢复后自动连回，
 *      并打印「网络已恢复（此前累计重试 M 次）」。超过 5 次则停止自动重连并给出
 *      排查建议（不会无限重连刷屏）。
 *   5) LED 指示（引脚来自 BSP 宏，非硬编码）：
 *        LED2 丝印 RX（IO8）常亮 = 已关联且已拿到 IP，熄灭 = 未连接；
 *        LED3 丝印 TX（IO9）每次 10 秒巡检翻转一次，作为「任务还活着」的心跳。
 *
 * 设计说明（架构取舍，评审时重点看这里）
 * --------------------------------------
 *   * Wi-Fi 事件回调运行在系统事件任务（sys_evt）上下文，因此回调里**只做**
 *     「记录状态 + 通知主任务」，绝不在这里延时/重连/扫描，避免阻塞整个事件循环。
 *   * 退避重连的计时放在本 demo 自己的任务里：主任务用
 *     ulTaskNotifyTake() 同时完成「等事件」和「等 10 秒巡检周期」两件事。
 *   * ESP32-C6 只有 2.4GHz 射频，扫描结果天然不含 5GHz AP，代码里不做额外过滤
 *     （仅打印提示说明这一事实）。
 *   * 扫描只在启动时做一次，连接成功后不再周期扫描 —— 因为 STA 在扫描期间会
 *     离开当前信道、造成短暂断流；周期扫描放在「只扫描模式」下使用。
 *   * 「是否可上网」用 esp_netif 状态 + IP/网关/DNS 是否有效做判据，**不引入
 *     额外组件**（没有用 esp_ping、没有开 raw socket），日志里明确标注这是推断
 *     而非实测。
 *   * 删除操作守则：本文件不含任何删除/擦除动作。NVS 异常时只打印告警与手动
 *     修复命令，绝不自动擦除 NVS。
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "demo_wifi_sta";

/* ===========================================================================
 * 参数（全部来自 menuconfig -> "EDK60 Demo 参数"）
 * =========================================================================== */

#define STA_SSID_CFG     CONFIG_APP_EDK60_WIFI_SSID      /* 目标 SSID，"":只扫描 */
#define STA_PASSWORD_CFG CONFIG_APP_EDK60_WIFI_PASSWORD  /* 目标密码 */
#define STA_SCAN_MAX_AP  CONFIG_APP_EDK60_WIFI_SCAN_MAX_AP

/* ===========================================================================
 * 本 demo 的常量
 * =========================================================================== */

#define STA_STATUS_PERIOD_MS 10000  /* 状态巡检 / 周期扫描周期 */
#define STA_RETRY_MAX        5      /* 断线自动重连次数上限（有限次，不无限刷屏） */
#define STA_RETRY_BASE_MS    2000   /* 退避基数：2s / 4s / 8s / 16s / 30s(封顶) */
#define STA_RETRY_CAP_MS     30000  /* 单次退避上限 */
#define STA_TOP_AP_COUNT     5      /* 只扫描模式下每次打印的最强 AP 个数 */
#define STA_TASK_STACK       6144   /* 栈：扫描表 + 事件处理 + printf，留足余量 */
#define STA_TASK_PRIO        4

/* 扫描表列宽（单位：显示列。ASCII 记 1 列，中文等非 ASCII 字符记 2 列） */
#define COL_ID    3
#define COL_SSID  26
#define COL_BSSID 17
#define COL_CH    4
#define COL_RSSI  5
#define COL_AUTH  18
#define COL_WPA3  4
#define TABLE_WIDTH                                                                                \
    (COL_ID + 2 + COL_SSID + 2 + COL_BSSID + 2 + COL_CH + 2 + COL_RSSI + 2 + COL_AUTH + 2 + COL_WPA3)

/* ===========================================================================
 * 运行状态
 * ===========================================================================
 * 写入方：Wi-Fi/IP 事件回调（系统事件任务）；读取方：wifi_sta_task。
 * 这里全部是字长对齐的标量，RV32 上的读写是原子的；语义上的竞态只在
 * 「事件刚发生、任务还没看到」的瞬间存在，对 demo 无影响。
 */
typedef enum {
    LINK_DOWN = 0,   /* 未关联 */
    LINK_ASSOCIATED, /* 已关联，等 DHCP 分配 IP */
    LINK_UP,         /* 已拿到 IP，链路可用 */
} link_phase_t;

static volatile link_phase_t s_link_phase = LINK_DOWN;
static volatile bool         s_connect_requested = false; /* 任务侧据此发起（重）连接 */
static volatile uint8_t      s_last_reason = 0;           /* 最近一次断开的 reason code */

static TaskHandle_t  s_main_task = NULL;
static esp_netif_t  *s_sta_netif = NULL;
static int64_t       s_start_us = 0;
static bool          s_led0_on = false;

/* ===========================================================================
 * 输出小工具：分隔线、UTF-8 宽度对齐的定宽列
 * =========================================================================== */

static void print_rule(int width, char ch)
{
    for (int i = 0; i < width; i++) {
        putchar(ch);
    }
    putchar('\n');
}

/* 计算 UTF-8 字符串的显示宽度：ASCII 记 1 列，其它字符（中文等）近似记 2 列。
 * 目的：SSID 里出现中文时，表格列仍然对齐。 */
static int utf8_disp_width(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if ((*p & 0xC0) != 0x80) { /* 只统计每个字符的首字节，跳过续字节 */
            w += (*p < 0x80) ? 1 : 2;
        }
    }
    return w;
}

/* 把 src 拷进 dst，最多占 max_cols 个显示列，且保证不切断 UTF-8 多字节字符 */
static void utf8_fit(char *dst, size_t dst_sz, const char *src, int max_cols)
{
    size_t o = 0;
    int w = 0;
    const unsigned char *p = (const unsigned char *)src;

    while (*p != '\0' && (o + 1) < dst_sz) {
        int len;
        if ((*p & 0x80) == 0x00) {
            len = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            len = 4;
        } else {
            len = 1; /* 非法首字节，按单字节处理，避免死循环 */
        }
        int cols = (len == 1) ? 1 : 2;
        if ((w + cols) > max_cols || (o + (size_t)len) >= dst_sz) {
            break;
        }
        for (int k = 0; k < len && p[k] != '\0'; k++) {
            dst[o++] = (char)p[k];
        }
        p += len;
        w += cols;
    }
    dst[o] = '\0';
}

/* 输出一个左对齐的定宽列（按显示宽度补空格） */
static void print_field(const char *s, int width)
{
    char buf[80];
    utf8_fit(buf, sizeof(buf), s, width);
    fputs(buf, stdout);
    for (int i = utf8_disp_width(buf); i < width; i++) {
        putchar(' ');
    }
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

/* ===========================================================================
 * Wi-Fi 术语的中文映射
 * =========================================================================== */

static const char *auth_mode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:                 return "OPEN(开放)";
    case WIFI_AUTH_WEP:                  return "WEP(已过时)";
    case WIFI_AUTH_WPA_PSK:              return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:             return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:         return "WPA/WPA2-PSK";
    case WIFI_AUTH_ENTERPRISE:           return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:             return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK:        return "WPA2/WPA3-SAE";
    case WIFI_AUTH_WAPI_PSK:             return "WAPI-PSK";
    case WIFI_AUTH_OWE:                  return "OWE(增强开放)";
    case WIFI_AUTH_WPA3_ENT_192:         return "WPA3-ENT-192";
    case WIFI_AUTH_DPP:                  return "DPP";
    case WIFI_AUTH_WPA3_ENTERPRISE:      return "WPA3-ENTERPRISE";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/WPA3-ENT";
    case WIFI_AUTH_WPA_ENTERPRISE:       return "WPA-ENTERPRISE";
    case WIFI_AUTH_UNKNOWN:              return "UNKNOWN";
    default:                             return "未知";
    }
}

/* 是否使用 WPA3（SAE）—— 给扫描表最后一列用 */
static bool auth_is_wpa3(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
        return true;
    default:
        return false;
    }
}

/* 断连原因码的中文释义（覆盖常见值，其余打印原始码） */
static const char *wifi_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:               return "未指明的原因";
    case WIFI_REASON_AUTH_EXPIRE:               return "认证过期（可能被 AP 主动剔除）";
    case WIFI_REASON_AUTH_LEAVE:                return "本机主动断开认证";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:return "长时间无流量被 AP 断开";
    case WIFI_REASON_ASSOC_TOOMANY:             return "AP 上关联的站点太多";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA: return "协议错误：未认证即发二层帧";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA: return "协议错误：未关联即发三层帧";
    case WIFI_REASON_ASSOC_LEAVE:               return "本机主动去关联（如调用 disconnect）";
    case WIFI_REASON_ASSOC_NOT_AUTHED:          return "关联但未通过认证";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:       return "对端功耗能力参数不可用";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:      return "对端不支持当前信道";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:   return "BSS 切换（漫游）";
    case WIFI_REASON_IE_INVALID:                return "管理帧信息元素非法";
    case WIFI_REASON_MIC_FAILURE:               return "MIC 校验失败（密钥不匹配？）";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:    return "四次握手超时（密码错误是最常见原因）";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:  return "组密钥更新超时";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:        return "四次握手中的信息元素不一致";
    case WIFI_REASON_GROUP_CIPHER_INVALID:      return "组播加密套件不可用";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:   return "单播加密套件不可用";
    case WIFI_REASON_AKMP_INVALID:              return "认证密钥管理协议不可用";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:     return "RSN 信息元素版本不支持";
    case WIFI_REASON_INVALID_RSN_IE_CAP:        return "RSN 能力字段非法";
    case WIFI_REASON_802_1X_AUTH_FAILED:        return "802.1X 认证失败";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:     return "加密套件被 AP 拒绝";
    case WIFI_REASON_TIMEOUT:                   return "连接超时";
    case WIFI_REASON_PEER_INITIATED:            return "对端发起断开";
    case WIFI_REASON_AP_INITIATED:              return "AP 发起断开";
    case WIFI_REASON_BEACON_TIMEOUT:            return "丢失 AP 信标（信号太弱或 AP 已关机）";
    case WIFI_REASON_NO_AP_FOUND:               return "没扫描到该 SSID 的 AP（SSID 写错 / 不在 2.4GHz / 太远）";
    case WIFI_REASON_AUTH_FAIL:                 return "认证失败（密码或认证方式不匹配）";
    case WIFI_REASON_ASSOC_FAIL:                return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:         return "握手超时（密码错误 / 信号差）";
    case WIFI_REASON_CONNECTION_FAIL:           return "连接失败（原因不明）";
    case WIFI_REASON_AP_TSF_RESET:              return "AP 时间同步基准复位";
    case WIFI_REASON_ROAMING:                   return "正常漫游切换";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "有该 SSID 但加密方式不兼容";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "有该 SSID 但低于认证方式门槛";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:     return "有该 SSID 但低于 RSSI 门槛";
    default:                                    return "见 esp_wifi_types.h 的 wifi_err_reason_t";
    }
}

static const char *rssi_quality_str(int8_t rssi)
{
    if (rssi >= -60) {
        return "优秀";
    }
    if (rssi >= -70) {
        return "良好";
    }
    if (rssi >= -80) {
        return "一般";
    }
    return "较弱（建议靠近 AP 或换信道）";
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

/* ===========================================================================
 * 事件回调：只记录状态 + 唤醒主任务，不做任何阻塞动作
 * =========================================================================== */

static void notify_main_task(void)
{
    if (s_main_task != NULL) {
        /* 事件回调运行在系统事件任务里（非中断上下文），用 xTaskNotifyGive 即可 */
        xTaskNotifyGive(s_main_task);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "[事件] WIFI_EVENT_STA_START —— STA 接口已启动，可以扫描/连接");
            notify_main_task();
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)event_data;
            ESP_LOGI(TAG, "[事件] WIFI_EVENT_STA_CONNECTED —— 关联成功: SSID=\"%.*s\" BSSID=" MACSTR
                          " 信道=%u 认证=%s AID=%u",
                     (int)e->ssid_len, (const char *)e->ssid, MAC2STR(e->bssid),
                     (unsigned)e->channel, auth_mode_str(e->authmode), (unsigned)e->aid);
            /* 关联成功但还没有 IP：等 IP_EVENT_STA_GOT_IP 再宣布"可用" */
            s_link_phase = LINK_ASSOCIATED;
            notify_main_task();
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)event_data;
            s_link_phase = LINK_DOWN;
            s_last_reason = e->reason;
            s_connect_requested = true; /* 由主任务做带退避的重连，不在回调里重连 */
            ESP_LOGW(TAG, "[事件] WIFI_EVENT_STA_DISCONNECTED —— 连接断开: reason=%u (%s)",
                     (unsigned)e->reason, wifi_reason_str(e->reason));
            notify_main_task();
            break;
        }

        default:
            ESP_LOGD(TAG, "[事件] WIFI_EVENT id=%d（本 demo 不专门处理）", (int)event_id);
            break;
        }
        return;
    }

    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)event_data;
            s_link_phase = LINK_UP;
            s_connect_requested = false; /* 已连上，取消重连请求 */
            ESP_LOGI(TAG, "[事件] IP_EVENT_STA_GOT_IP —— 拿到 IP: " IPSTR " 掩码: " IPSTR " 网关: " IPSTR
                          "（IP 是否变化: %s）",
                     IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw),
                     e->ip_changed ? "是" : "否");
            notify_main_task();
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            s_link_phase = LINK_DOWN;
            ESP_LOGW(TAG, "[事件] IP_EVENT_STA_LOST_IP —— IP 已丢失，等待重新获取");
            notify_main_task();
        }
        return;
    }
}

/* ===========================================================================
 * 扫描：执行一次并打印结果表
 * =========================================================================== */

/* RSSI 从强到弱排序（插入排序，n <= 60，稳定且不额外占内存） */
static void sort_by_rssi_desc(wifi_ap_record_t *recs, uint16_t n)
{
    for (uint16_t i = 1; i < n; i++) {
        wifi_ap_record_t key = recs[i];
        int j = (int)i - 1;
        while (j >= 0 && recs[j].rssi < key.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }
}

static void print_scan_table(const wifi_ap_record_t *recs, uint16_t n, uint16_t total, bool top_only)
{
    printf("\n");
    print_rule(TABLE_WIDTH, '=');
    if (top_only) {
        printf("【2.1 wifi_sta】周期扫描（每 %d 秒）—— 本区域内共 %u 个 AP，按 RSSI 排序显示最强 %u 个\n",
               STA_STATUS_PERIOD_MS / 1000, (unsigned)total, (unsigned)n);
    } else {
        printf("【2.1 wifi_sta】首次扫描 —— 共发现 %u 个 AP，最多显示 %u 条\n",
               (unsigned)total, (unsigned)STA_SCAN_MAX_AP);
    }
    printf("列说明：ID=序号 | CH=信道(2.4GHz 为 1~13) | RSSI=信号强度(dBm，越接近 0 越强)\n");
    printf("        AUTH=认证/加密方式 | WPA3=是否为 WPA3-SAE(是/否)\n");
    print_rule(TABLE_WIDTH, '-');

    /* 表头（与数据行使用同一套列宽，确保对齐） */
    printf("%*s  ", COL_ID, "ID");
    print_field("SSID", COL_SSID);
    printf("  %-*s  %-*s  %*s  ", COL_BSSID, "BSSID", COL_CH, "CH", COL_RSSI, "RSSI");
    print_field("AUTH", COL_AUTH);
    printf("  ");
    print_field("WPA3", COL_WPA3);
    printf("\n");
    print_rule(TABLE_WIDTH, '-');

    for (uint16_t i = 0; i < n; i++) {
        const wifi_ap_record_t *r = &recs[i];
        char ssid[40];
        int slen = (int)strnlen((const char *)r->ssid, sizeof(r->ssid));

        if (slen <= 0) {
            /* show_hidden=true，隐藏 SSID 的 AP 也会被扫到，此时 ssid 为空 */
            snprintf(ssid, sizeof(ssid), "%s", "<隐藏 SSID>");
        } else {
            snprintf(ssid, sizeof(ssid), "%.*s", slen, (const char *)r->ssid);
        }

        printf("%*u  ", COL_ID, (unsigned)(i + 1));
        print_field(ssid, COL_SSID);
        printf("  %02X:%02X:%02X:%02X:%02X:%02X  %*u  %*d  ",
               r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5],
               COL_CH, (unsigned)r->primary, COL_RSSI, (int)r->rssi);
        print_field(auth_mode_str(r->authmode), COL_AUTH);
        printf("  ");
        print_field(auth_is_wpa3(r->authmode) ? "是" : "否", COL_WPA3);
        printf("\n");
    }
    print_rule(TABLE_WIDTH, '=');
    printf("备注：ESP32-C6 射频只支持 2.4GHz Wi-Fi 6(802.11ax)，5GHz AP 不会出现在上表中。\n");
}

/*
 * 执行一次主动扫描并打印。
 *   top_only = false：按驱动返回顺序打印前 STA_SCAN_MAX_AP 条；
 *   top_only = true ：按 RSSI 排序后打印最强的 STA_TOP_AP_COUNT 条。
 * 返回 ESP_OK 表示扫描与打印都正常。
 */
static esp_err_t run_scan_and_report(bool top_only)
{
    /* 主动扫描（active）：发 Probe Request 更快更准；C6 无需区分频段 */
    wifi_scan_config_t sc = { 0 };
    sc.ssid = NULL;                    /* NULL = 不过滤 SSID */
    sc.bssid = NULL;                   /* NULL = 不过滤 BSSID */
    sc.channel = 0;                    /* 0 = 扫描全部信道 */
    sc.show_hidden = true;             /* 连隐藏 SSID 的 AP 一起扫出来 */
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 0;       /* 0 = 驱动默认单信道最短驻留时间 */
    sc.scan_time.active.max = 120;     /* 单信道最长 120ms，控制一次扫描的总时长 */

    esp_err_t err = esp_wifi_scan_start(&sc, true); /* true = 阻塞等到扫描结束（本任务自己等） */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描失败：esp_wifi_scan_start() 返回 %s", esp_err_to_name(err));
        return err;
    }

    uint16_t total = 0;
    err = esp_wifi_scan_get_ap_num(&total);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取扫描数量失败：%s", esp_err_to_name(err));
        esp_wifi_clear_ap_list(); /* 及时释放驱动内部结果，避免占内存 */
        return err;
    }

    if (total == 0) {
        printf("\n");
        print_rule(TABLE_WIDTH, '=');
        printf("【2.1 wifi_sta】扫描完成：没有发现任何 AP\n");
        printf("排查建议：确认路由器工作在 2.4GHz、SSID 未隐藏、天线区域远离金属遮挡。\n");
        print_rule(TABLE_WIDTH, '=');
        return ESP_OK;
    }

    uint16_t want = total;
    if (want > (uint16_t)STA_SCAN_MAX_AP) {
        want = (uint16_t)STA_SCAN_MAX_AP;
    }

    /* 扫描记录较大（每条上百字节），放堆上，避免撑爆任务栈 */
    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc((size_t)want, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        ESP_LOGE(TAG, "扫描结果缓冲区分配失败（%u 条，约 %u 字节）",
                 (unsigned)want, (unsigned)(want * sizeof(wifi_ap_record_t)));
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = want;
    err = esp_wifi_scan_get_ap_records(&got, recs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取扫描记录失败：%s", esp_err_to_name(err));
        free(recs);
        esp_wifi_clear_ap_list();
        return err;
    }

    if (got < total) {
        /* 只取走了一部分：剩下的记录仍留在 Wi-Fi 内部，必须主动清掉 */
        esp_wifi_clear_ap_list();
    }

    if (top_only) {
        sort_by_rssi_desc(recs, got);
        if (got > (uint16_t)STA_TOP_AP_COUNT) {
            got = (uint16_t)STA_TOP_AP_COUNT;
        }
    }

    print_scan_table(recs, got, total, top_only);

    free(recs);
    return ESP_OK;
}

/* ===========================================================================
 * 状态巡检打印
 * =========================================================================== */

static void print_link_status(int retry_used, bool gave_up)
{
    int64_t up_s = (esp_timer_get_time() - s_start_us) / 1000000;

    printf("\n");
    print_rule(TABLE_WIDTH, '=');
    printf("【2.1 wifi_sta】状态巡检（每 %d 秒一次）| 已运行 %lld 秒 | 链路=",
           STA_STATUS_PERIOD_MS / 1000, (long long)up_s);

    if (s_link_phase == LINK_UP) {
        wifi_ap_record_t ap = { 0 };
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_dns_info_t dns = { 0 };
        char ssid[40];

        printf("已连接(有 IP)\n");
        print_rule(TABLE_WIDTH, '-');

        int slen = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            slen = (int)strnlen((const char *)ap.ssid, sizeof(ap.ssid));
        } else {
            ESP_LOGW(TAG, "读取 AP 信息失败（esp_wifi_sta_get_ap_info），改用 Kconfig 中的 SSID");
        }
        if (slen > 0) {
            snprintf(ssid, sizeof(ssid), "%.*s", slen, (const char *)ap.ssid);
        } else {
            snprintf(ssid, sizeof(ssid), "%s", STA_SSID_CFG);
        }

        printf("  [关联] SSID=\"%s\"  BSSID=" MACSTR "  信道=%u  认证=%s\n",
               ssid, MAC2STR(ap.bssid), (unsigned)ap.primary, auth_mode_str(ap.authmode));
        printf("  [信号] RSSI=%d dBm（%s）\n", (int)ap.rssi, rssi_quality_str(ap.rssi));

        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            printf("  [网络] IPv4=" IPSTR "  掩码=" IPSTR "  网关=" IPSTR "\n",
                   IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));

            bool have_ip = (ip.ip.addr != 0);
            bool have_gw = (ip.gw.addr != 0);

            if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                printf("  [DNS ] 主服务器=" IPSTR "\n", IP2STR(&dns.ip.u_addr.ip4));
            } else {
                printf("  [DNS ] 未取到（esp_netif_get_dns_info 失败）\n");
            }
            bool have_dns = (dns.ip.u_addr.ip4.addr != 0);
            bool netif_up = esp_netif_is_netif_up(s_sta_netif);

            esp_netif_dhcp_status_t dhcpc = ESP_NETIF_DHCP_INIT;
            if (esp_netif_dhcpc_get_status(s_sta_netif, &dhcpc) != ESP_OK) {
                dhcpc = ESP_NETIF_DHCP_INIT;
            }
            printf("  [网卡] esp_netif 状态=%s，DHCP 客户端=%s\n",
                   netif_up ? "UP" : "DOWN", dhcp_status_str(dhcpc));

            if (have_ip && have_gw && have_dns && netif_up) {
                printf("  [判据] 已取得 IPv4 + 网关 + DNS 且网卡 UP -> 认为已接入局域网\n");
            } else {
                printf("  [判据] 网络要素不完整（IP=%d 网关=%d DNS=%d 网卡=%d）-> 尚未就绪\n",
                       (int)have_ip, (int)have_gw, (int)have_dns, (int)netif_up);
            }
            printf("         说明：本 demo 不引入额外组件（未做 ICMP ping / DNS 解析探测），\n");
            printf("               \"能否出公网\"请以上层应用（如 MQTT/HTTP）实测为准。\n");
        } else {
            printf("  [网络] 读取 IP 信息失败（esp_netif_get_ip_info）\n");
        }

    } else if (s_link_phase == LINK_ASSOCIATED) {
        printf("已关联但还没有 IP（等待 DHCP 分配）\n");
        print_rule(TABLE_WIDTH, '-');
        printf("  [说明] 已和 AP 完成认证/关联，正在等 DHCP 应答；若长时间停在这一步，\n");
        printf("         请检查路由器是否关闭了 DHCP、地址池是否已耗尽。\n");
        printf("         已经过去的秒数：%lld（DHCP 通常 < 2 秒完成）\n", (long long)up_s);
    } else {
        printf("未连接\n");
        print_rule(TABLE_WIDTH, '-');
        printf("  [原因] 最近一次断开 reason=%u（%s）\n",
               (unsigned)s_last_reason, wifi_reason_str(s_last_reason));
        if (gave_up) {
            printf("  [重试] 已用完 %d 次自动重连，已停止重试（复位或检查配置后再试）\n", STA_RETRY_MAX);
        } else {
            printf("  [重试] 已用 %d/%d 次，后续按 2s/4s/8s/16s/30s 退避重试\n",
                   retry_used, STA_RETRY_MAX);
        }
    }
    print_rule(TABLE_WIDTH, '=');
}

/* ===========================================================================
 * 初始化
 * =========================================================================== */

static esp_err_t wifi_init_once(void)
{
    /* 1) NVS：Wi-Fi 驱动用它保存校准数据/配置。
     *    注意：这里刻意不调用 nvs_flash_erase()（不自动擦除任何分区），
     *    只把修复动作和使用者说明打出来。 */
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

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "创建默认 STA 网络接口失败（esp_netif_create_default_wifi_sta）");
        return ESP_FAIL;
    }

    /* 3) Wi-Fi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败：%s", esp_err_to_name(err));
        return err;
    }

    /* 4) 注册事件：WIFI_EVENT 全部 + IP_EVENT 全部（回调内部按需分流） */
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

    /* 5) 模式 + STA 配置 */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA) 失败：%s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wc = { 0 };
    copy_cstr(wc.sta.ssid, sizeof(wc.sta.ssid), STA_SSID_CFG);
    copy_cstr(wc.sta.password, sizeof(wc.sta.password), STA_PASSWORD_CFG);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; /* 全信道扫描，便于挑到最强的那个 AP */
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN; /* 不做认证方式门槛过滤（最宽松） */
    wc.sta.threshold.rssi = -127;               /* 不做 RSSI 门槛过滤 */
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;     /* WPA3 同时支持 H2E 与 hunt-and-peck */

    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) 失败：%s", esp_err_to_name(err));
        return err;
    }

    /* 6) 关掉省电模式：本 demo 关心的是「状态刷新及时」，不是功耗 */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) 返回 %s（不影响功能，继续）", esp_err_to_name(ps_err));
    }

    /* 7) 启动 */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败：%s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* ===========================================================================
 * 主任务
 * =========================================================================== */

static uint32_t backoff_delay_ms(int retry_index)
{
    /* retry_index 从 0 开始：2s / 4s / 8s / 16s / 32s -> 封顶 30s */
    uint32_t d = (uint32_t)STA_RETRY_BASE_MS << retry_index;
    if (d > (uint32_t)STA_RETRY_CAP_MS) {
        d = (uint32_t)STA_RETRY_CAP_MS;
    }
    return d;
}

static void wifi_sta_task(void *arg)
{
    (void)arg;
    s_main_task = xTaskGetCurrentTaskHandle();
    s_start_us = esp_timer_get_time();

    bool has_target = (STA_SSID_CFG[0] != '\0');
    uint8_t mac[6] = { 0 };

    printf("\n");
    print_rule(TABLE_WIDTH, '=');
    printf("EDK60 Demo 2.1 —— wifi_sta 启动\n");
    printf("  板卡  : %s（%s）\n", EDK60_BOARD_NAME, EDK60_BOARD_REV);
    printf("  模组  : %s，Flash %d MB，无 PSRAM\n", EDK60_MODULE_NAME, EDK60_FLASH_SIZE_MB);
    printf("  射频  : ESP32-C6 集成 2.4GHz Wi-Fi 6(802.11ax)，仅 2.4GHz（无 5GHz）\n");
    printf("  模式  : %s\n", has_target ? "连接模式（先扫描，再连接 Kconfig 配置的 SSID）"
                                       : "只扫描模式（Kconfig 中 SSID 为空）");
    printf("  参数  : 显示上限 %d 条/次，巡检周期 %d ms，最大重连 %d 次\n",
           STA_SCAN_MAX_AP, STA_STATUS_PERIOD_MS, STA_RETRY_MAX);
    printf("  指示  : LED2 RX(IO%d) 常亮=已连接；LED3 TX(IO%d) 每 %d 秒翻转=心跳\n",
           edk60_led_pin(0), edk60_led_pin(1), STA_STATUS_PERIOD_MS / 1000);
    print_rule(TABLE_WIDTH, '=');

    ESP_LOGI(TAG, "开始初始化：NVS -> esp_netif -> 事件循环 -> esp_wifi -> start");
    if (wifi_init_once() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败，本 demo 结束（本任务退出，不影响其它任务）");
        vTaskDelete(NULL); /* 只结束本任务自身，不涉及任何数据删除 */
        return;
    }

    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "STA MAC = " MACSTR, MAC2STR(mac));
    }

    /* 等 WIFI_EVENT_STA_START（回调里会给一次通知），最多 5 秒 */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Wi-Fi 已启动，开始首次主动扫描（扫描期间本任务阻塞等待，事件回调不受影响）");

    /* ---- 首次扫描 ---- */
    if (run_scan_and_report(false) != ESP_OK) {
        ESP_LOGW(TAG, "首次扫描失败，%d 秒后自动重试一次", STA_STATUS_PERIOD_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(STA_STATUS_PERIOD_MS));
        (void)run_scan_and_report(false);
    }

    /* ---- 分支 A：未配置 SSID -> 只扫描 ---- */
    if (!has_target) {
        printf("\n");
        ESP_LOGI(TAG, "未配置目标 SSID（menuconfig -> EDK60 Demo 参数 -> APP_EDK60_WIFI_SSID），"
                      "进入「只扫描模式」：每 %d 秒重扫一次，打印最强的 %d 个 AP。",
                 STA_STATUS_PERIOD_MS / 1000, STA_TOP_AP_COUNT);
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(STA_STATUS_PERIOD_MS));
            (void)run_scan_and_report(true);
            edk60_led_toggle(1); /* 心跳：说明任务在跑、扫描在循环 */
        }
    }

    /* ---- 分支 B：连接模式 ---- */
    ESP_LOGI(TAG, "开始连接 SSID=\"%s\"（密码长度 %u 字符，%s）", STA_SSID_CFG,
             (unsigned)strlen(STA_PASSWORD_CFG),
             (strlen(STA_PASSWORD_CFG) == 0) ? "按开放网络尝试" : "按 WPA/WPA2/WPA3 个人版尝试");

    s_connect_requested = true; /* 统一由状态机判定是否需要连接 / 重连 */
    s_link_phase = LINK_DOWN;
    s_last_reason = 0;

    int  retry_used = 0;
    bool gave_up = false;

    /* 首次连接直接发起：不计入重试次数、不需要退避等待。
     * 若这个调用本身直接失败（不会产生 DISCONNECTED 事件），就自通知一次，
     * 让状态机立刻按退避策略接管。 */
    ESP_LOGI(TAG, "发起首次连接 ...");
    if (esp_wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "首次 esp_wifi_connect() 直接返回错误，改由退避重试流程处理");
        xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    }

    while (1) {
        /* 一次等待同时承担两件事：事件到来（返回 >0）或 10 秒巡检超时（返回 0） */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STA_STATUS_PERIOD_MS));

        /* --- 已连上有 IP --- */
        if (s_link_phase == LINK_UP) {
            if (retry_used != 0) {
                ESP_LOGI(TAG, "网络已恢复：此前累计重试 %d 次，现在已重新拿到 IP", retry_used);
            }
            retry_used = 0;
            gave_up = false;
            if (!s_led0_on) {
                edk60_led_set(0, true); /* 连接指示灯常亮 */
                s_led0_on = true;
            }
            if (notified == 0) {
                print_link_status(retry_used, gave_up);
                edk60_led_toggle(1); /* 心跳 */
            }
            continue;
        }

        /* --- 不需要（重）连：只做周期提示 --- */
        if (!s_connect_requested) {
            if (s_led0_on) {
                edk60_led_set(0, false);
                s_led0_on = false;
            }
            if (notified == 0) {
                print_link_status(retry_used, gave_up);
            }
            continue;
        }

        /* --- 断开/未连上：检查重试预算 --- */
        if (retry_used >= STA_RETRY_MAX) {
            if (!gave_up) {
                gave_up = true;
                s_connect_requested = false;
                ESP_LOGE(TAG, "连续 %d 次重连均失败，停止自动重连。最后 reason=%u（%s）",
                         STA_RETRY_MAX, (unsigned)s_last_reason, wifi_reason_str(s_last_reason));
                ESP_LOGE(TAG, "排查建议：1) SSID/密码是否正确（注意区分大小写、2.4GHz 与 5GHz 同名 AP）；"
                              "2) 路由器是否设置了 MAC 白名单/接入限制；3) 板子是否离 AP 太远。");
            }
            if (s_led0_on) {
                edk60_led_set(0, false);
                s_led0_on = false;
            }
            if (notified == 0) {
                print_link_status(retry_used, gave_up);
            }
            continue;
        }

        uint32_t wait_ms = backoff_delay_ms(retry_used);
        ESP_LOGW(TAG, "第 %d/%d 次重连：%u ms 后退避重试（上次 reason=%u %s）",
                 retry_used + 1, STA_RETRY_MAX, (unsigned)wait_ms,
                 (unsigned)s_last_reason, wifi_reason_str(s_last_reason));

        /* 等退避期间如果来了新事件（比如已经连上），立刻回到循环顶部重新判断 */
        uint32_t early = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        if (early > 0) {
            ESP_LOGI(TAG, "退避等待期间收到 Wi-Fi 事件，提前重新判断链路状态");
            continue;
        }

        ESP_LOGI(TAG, "发起连接（第 %d 次尝试）...", retry_used + 1);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            /* 失败时不会产生 DISCONNECTED 事件，这里补一次请求让状态机继续 */
            ESP_LOGE(TAG, "esp_wifi_connect() 返回 %s，将在下一次退避后重试", esp_err_to_name(err));
            s_connect_requested = true;
        }
        retry_used++;
    }
}

/* ===========================================================================
 * demo 入口
 * =========================================================================== */

void demo_wifi_sta_start(void)
{
    ESP_LOGI(TAG, "=== 2.1 wifi_sta：Wi-Fi STA 扫描/连接/状态信息 ===");

    /* LED 作为状态指示：用 BSP 的接口，引脚编号不落在这里硬编码 */
    ESP_ERROR_CHECK(edk60_led_init());
    edk60_led_set_all(false);

    BaseType_t ok = xTaskCreate(wifi_sta_task, "wifi_sta", STA_TASK_STACK, NULL, STA_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 wifi_sta 任务失败（栈 %d 字节），demo 无法运行", STA_TASK_STACK);
        return;
    }
    ESP_LOGI(TAG, "wifi_sta 任务已创建（栈 %d 字节，优先级 %d）。"
                  "首次扫描与连接过程请见下方日志。",
             STA_TASK_STACK, STA_TASK_PRIO);
    ESP_LOGI(TAG, "提示：SSID/密码在 menuconfig -> \"EDK60 Demo 参数\" 中配置；"
                  "SSID 留空则只扫描不连接。");
}
