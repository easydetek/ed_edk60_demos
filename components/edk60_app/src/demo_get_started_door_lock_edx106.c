/*
 * EDK60 Demo 5.5 —— door_lock_edx106（EDX106 门锁感应雷达 · I2C 主控 + OUT 电平状态机）
 * ===========================================================================
 * 30 秒看懂这个 demo
 * ---------------------------------------------------------------------------
 *   干什么 : 用 ESP32-C6 当**主控**，通过 I2C 配置 EDX106 模组里的
 *            AT10LP1T1RDB 10.5 GHz 雷达芯片（7 位地址 0x28），然后监测它的
 *            OUT 引脚电平（检测到目标 = 输出高电平），把「触发事件 + 配置状态」
 *            以**一行一条的文本**通过原生 USB Serial/JTAG 报给上位机；
 *            同时提供一组 USB 文本诊断命令（CFG? / SCAN / TRIG? / SET / RESET）。
 *   不干什么: **不驱动任何锁控 GPIO**（用户 2026-10-26 拍板：只上报 + 橙灯指示）；
 *            **不做 UART 透传**（本 demo 与 5.1~5.4 的"桥"完全不同）；
 *            不解析任何雷达原始波形（芯片自己在内部做判决，只给我们一根 OUT）。
 *
 * ===========================================================================
 * 与前面几个 demo 的本质差异（这是本文件为什么比它们"更像固件"的原因）
 * ===========================================================================
 *   | 维度     | 5.1~5.4（EDV151/EDV11P/EDV163…） | **5.5 EDX106（本 demo）**      |
 *   |----------|----------------------------------|-------------------------------|
 *   | 数据通路 | UART → USB，MCU 只搬字节          | **I2C 读写寄存器 + GPIO 中断** |
 *   | 协议位置 | 全在上位机                        | **在 MCU**（寄存器语义/时序/状态机）|
 *   | 配置保持 | 无需配置                          | **芯片掉电不保存 → 每次上电必须重配** |
 *   | 失败模式 | 丢字节（可自愈）                  | **配错一位就静默不生效**（最难查）  |
 *   ⇒ 所以本 demo 的重点不是"搬数据"，而是：
 *       ① 严格的启动时序与**逐字节回读校验**；
 *       ② 把"配置是否真的生效"变成**可观测的输出**（每个关键寄存器都打印实际值）；
 *       ③ ISR 里绝不碰 I2C / 日志（只用队列），电平判定以**轮询电平为准**。
 *
 * ===========================================================================
 * 硬件事实（接线由用户 2026-10-26 拍板，见 docs/project/edx106/plan.md §7.4）
 * ===========================================================================
 *   EDX106 只有 5 个脚：V / G / SCL / SDA / O（型号 EDX106-N-01，N = 无光敏）
 *     · **没有** delay pin、**没有** burst_en pin、**没有** light_in（光敏）脚、
 *       **没有**复位脚 → 这三个"外部 pin 控制"的档位在模组内部怎么绑的**未知**
 *       （硬件列为待实测 T-12）⇒ 本固件**强制走寄存器控制**（见下面 0x6c/0x6e/0x45）。
 *
 *   | 信号      | EDK60 落脚点            | GPIO  | 备注 |
 *   |-----------|------------------------|-------|------|
 *   | SCL       | H3 的 `T` 脚（原模组 TX）| IO4   | 走 GPIO matrix 的硬件 I2C |
 *   | SDA       | H3 的 `A` 脚（原 MCU TX）| IO5   | 同上 |
 *   | OUT       | H3 的 `A2`              | IO2   | 高有效；**输入 + 内部上拉，任何时刻都不驱动** |
 *   | V / G     | H3 的 `V` / `G`         | —     | 3.3V_RADAR，**由 IO14 使能**（必须拉高）|
 *
 *   · 雷达电源：`EDK60_PIN_RADAR_PWR_EN` = **IO14**（实测）；`IO14` 输出高 =
 *     3.3V_RADAR 上电（丝印 RADAR 红灯亮）。IO15 只是橙灯（丝印 L），与电源无关。
 *   · ⚠ **IO2 是 strapping 关注脚** + **模组未上电时 OUT 是高阻**（3V3_RADAR 由
 *     IO14 使能，固件拉高之前模组没电）⇒ 本固件的做法是：
 *       ① **先**把 IO2 配成「输入 + 内部上拉」（`GPIO_PULLUP_ONLY` 语义：只上拉、
 *          不下拉、绝不驱动），让引脚从第一刻起就有确定电平（= 高 = 无触发）；
 *       ② **再**拉高 IO14 给模组上电。顺序写死在 `demo_door_lock_edx106_start()`。
 *     ⇒ 因为从不驱动该脚，所以不会影响 C6 的启动模式。
 *   · ⚠ **Plan B（现场出现启动异常时用）**：把 OUT 改到 H3 的 `A3` = **IO3**
 *     （同属 GPIO0~7 / EXT1 唤醒域，且**不是 strapping 脚**、板上无功能网络占用）。
 *     改动只有两步：Kconfig `OUT_GPIO=3` + 把模组 OUT 线挪到 H3 的 A3。
 *   · ⚠ **IO2 属于 GPIO0~7**，将来产品化要做「有人靠近才唤醒 MCU」时，
 *     它可以落在 EXT1 深睡唤醒域里（本 demo 不定深睡，仅记下这个好处）。
 *   · ⚠ **I2C 上拉（2026-10-26 用户变更：样机阶段无外部电阻，全程用 C6 内部上拉）**：
 *     板上 R13/R14（4.7 kΩ → 3V3 → SCL/SDA）是 **DNP 未贴片**，而且它们挂在
 *     IO6/IO7 网络上，**与本次用的 IO4/IO5 无关**；用户手上也没有 4.7 k/10 k 电阻。
 *     ⇒ 现阶段全部依赖 **C6 内部上拉（~45 kΩ 量级）**，因此：
 *        · 总线频率默认 **100 kHz**（400 kHz 下上升沿几乎必然不达标 → NACK）；
 *        · 代码里 `flags.enable_internal_pullup = true`（SDA/SCL 都靠它）；
 *        · 探测失败时**逐级降频**：配置值 → 100 kHz → 50 kHz，并打印实际用的频率。
 *     ⚠ 这是**调试/样机**的可行做法，不是产品方案：量产建议**外置 2×4.7 kΩ**
 *     并把频率升回 400 kHz（芯片上限）。本 demo 的降频逻辑不会妨碍那次升级。
 *
 * ===========================================================================
 * 芯片寄存器事实（AN-003《AT10LP1T1RDB 寄存器读写说明》V1.2，已逐页核实）
 * ===========================================================================
 *   · I2C 从机，7 位地址 **0x28**（8 位写 0x50 / 读 0x51），**最高 400 kHz**
 *   · 111 字节寄存器，偏移 **0x00~0x6f**，连续读写地址自动加一
 *   · **单字节读必须 repeated START**（写 offset → Sr → 读），
 *     本固件用 `i2c_master_transmit_receive()` 一次事务完成，正好满足
 *   · **配置掉电不保存** → 每次上电都必须重新写入
 *   · 设备不在时读回 0xFF 或直接 NACK → **必须 probe 成功才继续**
 *   · 上电自检期间 OUT **强制高约 3000 ms**（0x3c/0x3d）→ 这段时间的事件必须丢弃
 *   · 软复位 `0x00=0x00 → 0x00=0x01`，复位期间 GPIO 为低
 *     （AN-003 §3.1：软复位**不改变**已写入的寄存器内容，只重载内部逻辑。
 *      本固件仍按「复位 → 释放 → 写全配置 → 回读 → 开中断」执行 —— 两种理解下都安全）
 *
 * ---------------------------------------------------------------------------
 * 本 demo 写入的配置块（顺序即写入顺序，注释即语义）
 * ---------------------------------------------------------------------------
 *   | 寄存器    | 值              | 语义 |
 *   |-----------|-----------------|------|
 *   | 0x00      | 0x00 → 0x01     | 软复位 → 释放（复位后重新写全部配置） |
 *   | **0x6c**  | **0xD3**        | bit7=1 超低功耗；**bit6=1 工作模式由寄存器**；bit4=1 关光敏；bits1:0=0b11 |
 *   | **0x6d**  | **0xF8**        | bit3=1 参数可调；bits2:0=000 **最低功耗档**；bits7:4 按厂商参考值保留 |
 *   | **0x6e**  | **0xE8**        | **bit6=1 阈值由寄存器**；bit5=1 burst 可调；bits4:3=01；bit7 按参考值保留 |
 *   | 0x0f/0x10 | thr&0xFF / (thr>>8)&0x03 | 感应阈值 0~1023（低 8 位 + 高 2 位） |
 *   | 0x45      | **0x01**        | **bit0=1 延时由寄存器控制**（默认 0 = 外部 delay pin） |
 *   | 0x46~0x49 | delay 的 32 位小端 | 高电平延时，**单位 ms**（默认 2000） |
 *   | 0x52/0x53 | lockout 的 16 位小端 | 灭灯保护（闭锁）时间，**单位 ms**（默认 2000） |
 *   | 0x60      | **0x33**        | 第一级接收增益（默认值；3 dB/step，**值越大增益越小**） |
 *
 * ── 三个"缺一不可"的寄存器控制位（本次核心，务必连同注释一起读） ────────────
 *   ① **0x6e bit6 = 1**：否则**阈值仍由外部 pin 的 8 档决定**，
 *      写进 0x0f/0x10 的值**根本不生效**（默认值 0x98 的 bit6 = 0！）。
 *      这是本 demo 最容易踩、且最难查的坑 —— 回读还会"看起来是对的"。
 *   ② **0x45 bit0 = 1**：否则延时仍由外部 delay pin 决定，0x46~0x49 不生效。
 *   ③ **0x6c bit6 = 1**：否则工作模式仍由外部 burst_en pin 决定。
 *   而 EDX106 模组**这三个 pin 一个都没引出**（5 脚模组）⇒ 不显式配置时
 *   真实行为未知（硬件待实测 T-12）⇒ 本固件三处全部强制置 1，并在启动日志里
 *   把这三个寄存器的**回读实际值**打印出来（`EDX106: ctrl ...` 那一行）。
 *   下面还有 `_Static_assert` 把"默认值必须逐字节等于厂商参考值 0xD3/0xF8/0xE8"
 *   钉死在编译期，防止后人改 Kconfig 时不知不觉改掉位型。
 *
 *   关于 0x6c bit4（关光敏）：本型号 `EDX106-N` **没有光敏脚**，该位实际无作用，
 *   但写 1 无害且能兼容有光敏的型号（`-Y`）⇒ 保留，标注"兼容保留"。
 *
 *   关于 0x6c 的 bits1:0：任务书里的语义写作"bit1:0=1"，但厂商参考值 0xD3 的
 *   bits[1:0] 实际是 **0b11**。本固件以**参考值的实际位型为准**（0xD3），
 *   不替手册猜这两位的作用 —— 代码里固定成 0b11 并在此说明差异。
 *
 * ===========================================================================
 * 启动序列（严格顺序，每一步失败都有明确的上报）
 * ===========================================================================
 *   1. `EDK60_PIN_RADAR_PWR_EN`(IO14) 拉高 → 等 ≥100 ms
 *      （**在它之前**先把 OUT 配成"输入 + 上拉"，让该脚从第一刻起就有确定电平）
 *   2. I2C 总线（SCL=IO4 / SDA=IO5，频率取 Kconfig，内部上拉）→ 挂 0x28
 *   3. `i2c_master_probe(0x28)`；失败 → **逐级降频重试**（配置值 → 100 kHz → 50 kHz），
 *      打印每次尝试与实际使用的频率；全失败 → 报 `I2C_NO_DEVICE` + 外置上拉建议，
 *      进**诊断模式**（不重启，保留 SCAN/RESET 便于排查）
 *   4. 读 0x0d（应为 0x0a）作为存在性探针，**打印实际值**（不一致只记录不中止）
 *   5. 软复位：写 0x00=0x00 → 10 ms → 写 0x00=0x01 → 等 20 ms
 *   6. 写配置块（0x6e 必须排在 0x0f/0x10 之前）
 *   7. **回读校验** 13 个寄存器，逐字节比对，不一致打印
 *      `EDX106: CFG_MISMATCH reg=0xNN wrote=0xNN read=0xNN`
 *   8. 配置 OUT(IO2) 为输入 + **双边沿中断**（ISR 内只投递队列，不碰 I2C、不打印）
 *   9. 自检窗口（`_SELFCHECK_MS`，默认 3000 ms，从模组**上电那一刻**起算）
 *      内丢弃全部触发事件，窗口结束打印 `EDX106: SELFCHECK done level=<n>`
 *  10. 进入监控循环（20 ms 节拍 + 中断唤醒；**电平以轮询为准**）
 *
 * ===========================================================================
 * USB 上报协议（每行一条，`\r\n` 结尾，便于后续网页/脚本解析）
 * ===========================================================================
 *   EDX106: t=<ms> trig=1 level=1 delay=<ms> lockout=<ms> thr=<n> mode=<normal|ulp>
 *   EDX106: led on (trigger)          ← 2026-10-18 新增（独立行；上一行字段一个没动）
 *   EDX106: t=<ms> trig=0 width=<实测高电平毫秒>
 *   EDX106: led off (release)         ← 2026-10-18 新增
 *   EDX106: stat interval=<s> trig=<n> width_min=<ms> width_max=<ms> i2c_err=<n>
 *   EDX106: cfg ok thr=.. delay=.. lockout=.. mode=.. light=off
 *   EDX106: run mode=<ulp|normal> thr=<n> delay=<ms> lockout=<ms> gain=0xNN(0x60) win=<len>/<total>/<cnt>
 *                                     ← 2026-10-18 新增：**回读值**，bring-up 末尾与每条成功 SET 之后
 *   EDX106: led init gpio=15 active_level=1 enable=1 level=<0|1> follow_out=<0|1>
 *   EDX106: led on|off gpio=15 level=<实测读回>     （手动命令的回执）
 *   EDX106: led? level=.. on=.. active_level=.. cfg_enable=.. trigger_led=.. follow_out=..
 *   EDX106: led blink done
 *   EDX106: led disabled (CONFIG_APP_EDK60_EDX106_LED_ENABLE=n)   （关灯编译时所有 LED 命令的回执）
 *   其余（诊断/错误）行同样以 `EDX106: ` 开头，按第一个 key 区分：
 *     `EDX106: start ...`    启动参数快照
 *     `EDX106: boot ...`     电源使能 + 等待
 *     `EDX106: probe ...` / `EDX106: i2c ok ...` / `EDX106: I2C_DOWNSHIFT ...`
 *     `EDX106: err <CODE> ...`  错误行（CODE ∈ I2C_NO_DEVICE / I2C_BUS_INIT /
 *                              ID_READ / CFG_WRITE / OUT_INIT / LED_INIT / PWR_INIT / NOT_READY）
 *     `EDX106: id reg0x0d=...`  存在性探针实际值
 *     `EDX106: ctrl ...`     三个强制控制寄存器的回读值
 *     `EDX106: CFG_MISMATCH reg=0xNN wrote=0xNN read=0xNN`
 *     `EDX106: SELFCHECK done ...`
 *     `EDX106: trig? ...` / `EDX106: reg ...` / `EDX106: scan ...` / `EDX106: set ...`
 *     `EDX106: help ...`
 *   ⚠ 注意 `stat` 行：任务书里写的是 `width_min/max=<ms>` 这种"两值一格"的简写，
 *     本实现拆成 `width_min=` 与 `width_max=` **两个独立键**（各自一个数字），
 *     否则解析器拿不到边界信息。这是**有意偏离**，已在实现说明里登记。
 *
 * ===========================================================================
 * 诊断命令（USB CDC 行输入；只读优先，写入命令必打印回读确认）
 * ===========================================================================
 *   CFG?   打印全部关键寄存器的 hex 与语义（逐条回读）
 *   SCAN   扫 0x08~0x77 找从机（本模组应出现在 0x28）
 *   TRIG?  当前 OUT 电平、最近一次触发时间戳、累计次数、轮询纠偏次数
 *   SET delay=<ms> thr=<0..1023> lockout=<ms>   写对应寄存器 + **回读确认**
 *   SET mode=ulp|normal   只翻 0x6c bit7（探测距离的第一杠杆，见 notes §16）
 *   SET gain=<0..15>      写 0x60 bit7:4（3 dB/step，值越大增益越小）
 *   SET win=<len>,<total>,<cnt>   写 0x0d / 0x30 / 0x31（降 cnt 更易触发）
 *   LED on / LED off / LED blink / LED?   **只动 IO15**（不碰 I2C、不影响 OUT 中断）
 *   RESET  重跑启动序列（软复位 + 重配 + 重新开自检窗口）
 *   未知命令 → 打印一行帮助
 *
 * ===========================================================================
 * 上板判据（60 秒看什么）
 * ===========================================================================
 *   ✅ 正常：`EDX106: i2c ok freq=...` → `EDX106: id reg0x0d=0x0a` →
 *           `EDX106: cfg ok ...` → `EDX106: ctrl 0x6c=0xd3 ... 0x6e=0xe8` →
 *           `EDX106: SELFCHECK done` → 手在 1 m 处挥动出现 `trig=1` / `trig=0`
 *   ❌ `EDX106: err I2C_NO_DEVICE`：内部上拉下 100 kHz 与 50 kHz 都失败 ⇒
 *      基本就是"内部上拉不够（需外置 2×4.7 kΩ）/ SCL-SDA 接反 / 3V3_RADAR 没电"三件事
 *      （固件会跟着打一行 `hint 建议加 2x4.7k 外置上拉`；`SCAN` 命令还会给三步排查法）
 *   ❌ `CFG_MISMATCH reg=0x6e`：说明阈值控制位没写进去 —— 阈值会完全不生效
 *   ❌ 一上电就 `trig=1` 且一直不落：自检窗口没等够（0x3c/0x3d 默认 3000 ms）
 *
 * ===========================================================================
 * 设计约束（本工程硬规则，改本文件前先读）
 * ===========================================================================
 *   · 本工程 **20 个 demo 的源文件一起编译**，任何无条件 `#error` 会让每次构建
 *     全部失败（历史坑 B1）⇒ 本文件所有断言都包在
 *     `#if defined(CONFIG_APP_EDK60_DEMO_DOOR_LOCK_EDX106)` 里。
 *   · `default n` 的 bool+`choice` 不产生宏（历史坑 A1/A2）⇒ 本文件对每个 bool
 *     都用 `#ifdef` 兜底成 0/1 宏，不在代码里直接写 `#if CONFIG_xxx`。
 *   · **不通篇使用 printf / ESP_LOGx**：本 demo 的输出要作为**文本协议**被解析，
 *     日志混进去会制造"坏行"。所有输出统一走 `usb_line()` → 只写 USB。
 *     （ESP_LOG 在本工程里会被复制到 USB 第二输出口 —— 所以另有
 *       `sdkconfig.defaults.door_lock_edx106` 把日志在编译期关掉，双保险。）
 * ===========================================================================
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "edk60_board.h"
#include "edk60_demo.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* ===========================================================================
 * 0. 编译期参数（Kconfig → C 宏）
 * ---------------------------------------------------------------------------
 * 三硬规则之二：`default n` 的 bool **不生成** `CONFIG_xxx` 宏，
 * 所以每个 bool 都要 `#ifdef` 兜底；否则关掉该项后编译直接报 undeclared。
 * =========================================================================== */

#ifdef CONFIG_APP_EDK60_EDX106_OUT_ACTIVE_HIGH
#define CFG_OUT_ACTIVE_HIGH 1
#else
#define CFG_OUT_ACTIVE_HIGH 0
#endif

#ifdef CONFIG_APP_EDK60_EDX106_ULP_MODE
#define CFG_ULP 1
#else
#define CFG_ULP 0
#endif

#ifdef CONFIG_APP_EDK60_EDX106_LIGHT_OFF
#define CFG_LIGHT_OFF 1
#else
#define CFG_LIGHT_OFF 0
#endif

#ifdef CONFIG_APP_EDK60_EDX106_LED_ENABLE
#define CFG_LED_ENABLE 1
#else
#define CFG_LED_ENABLE 0
#endif

/* 橙灯行为（2026-10-18 用户新增"只要触发后亮灯"的需求）：
 *   1 = 跟随 OUT 高电平（灯亮多久由芯片 delay 决定，固件不做定时）；
 *   0 = 触发时**闪一下**（LED_FLASH_MS 后由 monitor 熄灭，时间戳判定，不用定时器中断）。
 * `default y` 的 bool 被关掉后同样不生成宏 ⇒ 一律 #ifdef 兜底，不在代码里写 #if CONFIG_xxx。 */
#ifdef CONFIG_APP_EDK60_EDX106_LED_FOLLOW_OUT
#define CFG_LED_FOLLOW_OUT 1
#else
#define CFG_LED_FOLLOW_OUT 0
#endif

#ifdef CONFIG_APP_EDK60_EDX106_SILENT_LOGS
#define CFG_SILENT_LOGS 1
#else
#define CFG_SILENT_LOGS 0
#endif

/* ===========================================================================
 * 1. 引脚与时序常量
 * =========================================================================== */

/** I2C SCL（用户拍板：H3 的 `T` 脚 = IO4，原模组 TX） */
#define PIN_SCL              ((gpio_num_t)CONFIG_APP_EDK60_EDX106_SCL_GPIO)
/** I2C SDA（用户拍板：H3 的 `A` 脚 = IO5，原 MCU TX） */
#define PIN_SDA              ((gpio_num_t)CONFIG_APP_EDK60_EDX106_SDA_GPIO)
/** OUT 输入（用户拍板：H3 的 `A2` = IO2；strapping 关注脚，只配输入） */
#define PIN_OUT              ((gpio_num_t)CONFIG_APP_EDK60_EDX106_OUT_GPIO)
/** 橙色指示灯（丝印 "L"）：实测 IO15，与雷达电源 EN(IO14) 无关 */
#define PIN_LED              EDK60_PIN_RADAR_LED
/** 雷达电源使能：实测 IO14，输出高 = 3.3V_RADAR 上电 */
#define PIN_PWR_EN           EDK60_PIN_RADAR_PWR_EN

/** 打开 3.3V_RADAR 后等待模组就绪的时间（手册未给 tPOR，取保守值 100 ms） */
#define RADAR_PWR_SETTLE_MS  (100)
/** 软复位时序：写 0x00=0x00 后等这么久，写 0x00=0x01 后再等 Release_ms */
#define SOFT_RESET_HOLD_MS   (10)
#define SOFT_RESET_RELEASE_MS (20)

/** I2C 从机地址（7 位）与访问参数 */
#define EDX106_I2C_ADDR_7BIT (0x28)
/** 降频重试的两档（内部上拉 ~45 kΩ 时，频率越低越容易 ACK）。
 *  语义：配置值 → 100 kHz（仅当配置值更高时）→ 50 kHz（仅当上一档更高时），
 *  最多 3 次；每次失败都会打印，最终打印**实际使用的频率**。 */
#define I2C_FREQ_STEP1_HZ    (100000)
#define I2C_FREQ_STEP2_HZ    (50000)
/** 单次 I2C 事务超时（ms）——400 kHz 下一字节约 22.5 µs，50 ms 已是 2000 倍余量 */
#define I2C_XFER_TIMEOUT_MS  (50)
/** 每次寄存器访问的重试次数（要求 ≥3） */
#define I2C_TRIES            (3)

/** 监控循环的轮询节拍（ms）。中断负责及时性，轮询负责"以电平为准"的纠偏 */
#define MON_POLL_MS          (20)

/** `LED_FOLLOW_OUT=n` 时"闪一下"的时长（ms）：上升沿点亮 → 到点由 monitor 熄灭。
 *  用时间戳判定（monitor 每 20 ms 一次），**不使用定时器/定时器中断**：
 *  定时器会引入中断上下文里的 GPIO 操作与临界区，而这里 20 ms 的精度完全够。 */
#define LED_FLASH_MS         (300)
/** `LED blink`：亮灭循环次数与每次时长（ms），用于"5 秒内确认灯到底能不能亮" */
#define LED_BLINK_TIMES      (3)
#define LED_BLINK_MS         (200)

/** USB 输出行缓冲（最长一行是 TRIG? 与 start 行，320 字节足够） */
#define USB_LINE_BUF         (320)
/** 命令行长缓冲 */
#define CMD_LINE_BUF         (128)

/* ===========================================================================
 * 2. 寄存器地址与配置值
 * =========================================================================== */

#define REG_RSTN_SW          (0x00)  /* 0x00 软复位 / 0x01 释放 */
#define REG_ID_PROBE         (0x0d)  /* signal_det_len，默认 0x0a —— 存在性探针 */
#define REG_THR_L            (0x0f)  /* signal_det_thr_1_1：阈值低 8 位 */
#define REG_THR_H            (0x10)  /* signal_det_thr_1_2：阈值高 2 位（bit1:0） */
/* ---- 判决窗口三兄弟（2026-10-18 新增 SET win，用于"更易触发"） ----
 * 0x0d 与上面的存在性探针是**同一个寄存器**（signal_det_len）。
 * 出厂值 0x0a 恰好也是我们拿来当"器件在位"的判据，所以：
 *   · 只要不执行 `SET win=`，0x0d 保持 0x0a，启动日志的 `id reg0x0d=0x0a` 不变；
 *   · 一旦改过 0x0d，重启后探针行会打成 `(mismatch, continue)` ——
 *     **那不代表器件有问题**，只代表窗口长度被改过（见 notes §15）。
 * 0x30 / 0x31 的出厂值本固件不写，只读回来打印（`run ... win=<len>/<total>/<cnt>`）。 */
#define REG_WIN_LEN          (0x0d)  /* 窗口长度（同时是存在性探针，见上） */
#define REG_WIN_TOTAL        (0x30)  /* 观察窗口总数 */
#define REG_WIN_CNT          (0x31)  /* 需超过阈值的窗口数（默认 3，降到 2 更易触发） */
#define REG_DELAY_SEL        (0x45)  /* bit0=1 延时由寄存器控制 */
#define REG_DELAY0           (0x46)  /* 延时 32 位小端，byte0 */
#define REG_DELAY1           (0x47)
#define REG_DELAY2           (0x48)
#define REG_DELAY3           (0x49)
#define REG_LOCKOUT_L        (0x52)  /* 灭灯保护（闭锁）时间低字节，单位 ms */
#define REG_LOCKOUT_H        (0x53)
#define REG_GAIN1            (0x60)  /* 第一级接收增益（3 dB/step，越大增益越小） */
#define REG_CNF_1            (0x6c)
#define REG_CNF_WORK_TIME    (0x6d)
#define REG_CNF_BURST_THR    (0x6e)

/** 0x0d 的期望值（手册默认 0x0a） */
#define REG_ID_PROBE_EXPECT  (0x0a)

/* ---- 0x6c 位型：默认必须逐字节等于厂商参考值 0xD3 ---- */
/** bits[1:0] 固定 0b11：复刻参考值 0xD3 的实际位型（任务书语义写作"bit1:0=1"，见文件头说明） */
#define REG_6C_BITS_1_0      (0x03)
/** bit6=1：**工作模式由寄存器控制**（模组没引出 burst_en pin，必须置 1） */
#define REG_6C_BIT_MODE_REG  (0x40)
/** bit4=1：关光敏（本型号无光敏脚，**兼容保留**） */
#define REG_6C_BIT_LIGHT_OFF (0x10)
/** bit7=1：超低功耗模式 */
#define REG_6C_BIT_ULP       (0x80)
/** **不含 bit7** 的基值：bits1:0=0b11 + bit6=1（模式走寄存器）+ 可选 bit4（关光敏）。
 *  拆出这一层是为了 `SET mode=normal|ulp` 能只翻 bit7、**不碰其余任何一位**：
 *    · 常规模式（0x6c bit7=0）⇒ LIGHT_OFF=y 时 = **0x53**
 *    · 超低功耗   （0x6c bit7=1）⇒ LIGHT_OFF=y 时 = **0xD3**（原默认值，逐字节不变） */
#define REG_6C_BASE                                                    \
    ((uint8_t)(REG_6C_BITS_1_0 | REG_6C_BIT_MODE_REG |                  \
               (CFG_LIGHT_OFF ? REG_6C_BIT_LIGHT_OFF : 0)))
#define REG_6C_VALUE                                                   \
    ((uint8_t)(REG_6C_BASE | (CFG_ULP ? REG_6C_BIT_ULP : 0)))
/** `SET mode=normal` 写这个（bit7=0） */
#define REG_6C_NORMAL_VALUE  ((uint8_t)(REG_6C_BASE))
/** `SET mode=ulp` 写这个（bit7=1） */
#define REG_6C_ULP_VALUE     ((uint8_t)(REG_6C_BASE | REG_6C_BIT_ULP))

/* ---- 0x6d 位型：默认必须等于 0xF8 ---- */
/** bits7:4 按厂商参考值保留（手册未在任务书范围内给出这四位的语义，不擅自改写） */
#define REG_6D_BITS_7_4      (0xF0)
/** bit3=1：参数可调（否则下面 bits2:0 的功耗档不生效） */
#define REG_6D_BIT_ADJ       (0x08)
/** bits2:0 = 000：**最低功耗档**（手册：改功耗会影响感应距离，需重新评估） */
#define REG_6D_PWR_STEP      (0x00)
#define REG_6D_VALUE \
    ((uint8_t)(REG_6D_BITS_7_4 | REG_6D_BIT_ADJ | (REG_6D_PWR_STEP & 0x07)))

/* ---- 0x6e 位型：默认必须等于 0xE8 ---- */
/** bit7 按厂商参考值保留（语义未在本次核实范围内） */
#define REG_6E_BIT7          (0x80)
/** bit6=1：**阈值由寄存器控制**（默认 0x98 的 bit6=0 是外部 pin！这是头号坑） */
#define REG_6E_BIT_THR_REG   (0x40)
/** bit5=1：burst 可调 */
#define REG_6E_BIT_BURST_ADJ (0x20)
/** bits4:3 = 01：burst 频率档（按参考值） */
#define REG_6E_BITS_4_3      (0x01)
#define REG_6E_VALUE                                       \
    ((uint8_t)(REG_6E_BIT7 | REG_6E_BIT_THR_REG |            \
               REG_6E_BIT_BURST_ADJ |                        \
               ((REG_6E_BITS_4_3 & 0x03) << 3)))

/** 0x45 的值：bit0=1 = 延时由寄存器控制（模组没引出 delay pin，必须置 1） */
#define REG_DELAY_SEL_VALUE  (0x01)
/** 0x60 第一级接收增益默认值（误触发多时往大调：值越大增益越小） */
#define REG_GAIN1_VALUE      (0x33)

/* ===========================================================================
 * 3. 运行时状态
 * ---------------------------------------------------------------------------
 * 并发模型（本 demo 只有两个任务，故意做得极简）：
 *
 *   task_monitor ── 独占 OUT 电平判定 + 橙灯 + 事件上报 + 周期统计（不碰 I2C）
 *   task_cmd     ── 独占 USB 行输入 + 全部 I2C 访问（CFG?/SCAN/SET/RESET）
 *
 *   · bring-up 第一次跑在 demo 入口（app_main 所在任务）里，**在 task_cmd 创建
 *     之前**完成，所以第一次 bring-up 与命令处理不会并发；
 *     之后 RESET 命令在 task_cmd 里重跑 bring-up —— 与第一次不重叠。
 *   · 结论：**任意时刻只有一个执行流在访问 I2C**，因此不需要 I2C 互斥锁
 *     （少一把锁就少一个死锁点）。若将来把 I2C 搬进监控循环，必须补锁。
 *   · USB 写是多任务共享的（monitor / cmd 都会写）→ 用 `s_usb_lock` 串行化，
 *     否则两行文本会互相插进对方中间，解析器直接坏掉。
 * =========================================================================== */

static i2c_master_bus_handle_t s_bus;   /**< I2C 主机总线（自建，不复用 BSP 的 IO6/IO7） */
static i2c_master_dev_handle_t s_dev;   /**< 0x28 从机句柄 */
static uint32_t s_i2c_freq_hz;          /**< **实际**使用的总线频率（降频后记录新值） */
static bool s_cfg_verified;             /**< 配置块回读校验是否全通过 */
static bool s_out_armed;                /**< OUT 中断是否已挂上（幂等保护） */

static SemaphoreHandle_t s_usb_lock;    /**< USB 写锁 */
static QueueHandle_t s_out_q;           /**< OUT 边沿队列（ISR → monitor） */

/** 自检窗口截止时刻（µs，esp_timer 时基）。从模组**上电/软复位**那一刻起算。
 *  volatile：monitor 读、入口/cmd 任务写；64 位读不是原子的，但这里语义上
 *  只要求"大致到点"，读到半新半旧的值最坏是提前/推后一个节拍，可接受。 */
static volatile int64_t s_selfcheck_until_us;
/** 自检窗口内被丢弃的事件数（每次重开窗口清零） */
static volatile uint32_t s_selfcheck_dropped;

/* 当前生效的配置（SET 命令会更新，上报的行用它，保证"看到的就是芯片里的"） */
static volatile uint32_t s_thr;
static volatile uint32_t s_delay_ms;
static volatile uint32_t s_lockout_ms;

/* 统计（monitor 写；i2c_err 由 I2C 封装写，故都是 volatile） */
static volatile uint32_t s_i2c_err;
static uint32_t s_trig_total;
static uint32_t s_stat_trig;
static uint32_t s_stat_wmin;
static uint32_t s_stat_wmax;
static uint32_t s_poll_resync;          /**< 中断与轮询不一致、由轮询纠偏的次数 */
static int64_t s_last_rise_us;          /**< 最近一次上升沿时刻（0 = 从未触发） */
static uint32_t s_last_width_ms;
static int64_t s_next_stat_us;

/* ---- 模式与橙灯的运行期状态（2026-10-18 新增） ----
 * s_mode_ulp：**芯片里实际生效的** 0x6c bit7（不是 Kconfig 值）。
 *   启动回读与 `SET mode=` 后都会刷新它；`trig=1` / `cfg ok` 两行的 `mode=` 字段取值来源
 *   从"编译期 CFG_ULP"改成它 —— **字段名与取值范围不变**，只是不再说谎（现场 SET 过模式后
 *   旧写法会一直报 ulp，容易误判"模式没切过去"）。
 * s_led_flash_off_us：`LED_FOLLOW_OUT=n` 时的"到点熄灭"时刻（µs；0 = 无待熄灭窗口）。
 *   monitor 写、task_cmd 手动命令清零 —— volatile 只为让编译器不缓存，64 位读非原子，
 *   最坏偏差一个 20 ms 节拍，语义上完全够（与 s_selfcheck_until_us 同一策略）。 */
static volatile bool s_mode_ulp;
static volatile int64_t s_led_flash_off_us;
static volatile bool s_led_by_trigger;   /**< 当前点亮是否由触发引起（LED? 的 trigger_led） */

/* 前置声明 */
static void usb_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ===========================================================================
 * 4. USB 输出（本 demo 唯一的对外通道）
 * =========================================================================== */

/** 原样写一段字节到 USB CDC（不加密、不加前缀）。失败时丢弃并计入 s_i2c_err 之外的
 *  独立路径 —— 这里不做重试，因为输出通道本身出问题时重试只会更慢。 */
static void usb_write_raw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (p == NULL || len == 0) {
        return;
    }
    if (s_usb_lock != NULL) {
        (void)xSemaphoreTake(s_usb_lock, portMAX_DELAY);
    }
    while (len > 0) {
        /* 20 ms 预算：USB 主机不读时不要长时间占住调用者（尤其 monitor 任务） */
        int wrote = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(20));
        if (wrote <= 0) {
            break;                      /* 主机没在收：丢掉这一段，绝不阻塞事件上报 */
        }
        p += wrote;
        len -= (size_t)wrote;
    }
    if (s_usb_lock != NULL) {
        (void)xSemaphoreGive(s_usb_lock);
    }
}

/**
 * 打印一行协议文本：自动加 `\r\n`、自动串行化（多任务安全）。
 * 用 `__attribute__((format(printf,1,2)))` 让编译器帮我们查格式串 ——
 * 本工程用 `-Werror` 构建，格式错会当场失败，而不是上板后打出乱码。
 */
static void usb_line(const char *fmt, ...)
{
    char buf[USB_LINE_BUF];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }

    size_t len;
    if ((size_t)n >= sizeof(buf) - 2) {
        len = sizeof(buf) - 3;          /* 截断，但仍保证下面两个字节放得下 */
    } else {
        len = (size_t)n;
    }
    buf[len++] = '\r';
    buf[len++] = '\n';
    usb_write_raw(buf, len);
}

/* ===========================================================================
 * 5. I2C 底层（全部带超时 + 3 次重试 + 失败计数）
 * ---------------------------------------------------------------------------
 * · 写单寄存器：{reg, val} 一次事务（STOP 由驱动自己发）
 * · **读单寄存器：`i2c_master_transmit_receive()` = 写 offset → repeated START → 读**
 *   —— 手册明确要求单字节读必须用重复起始，不能"写 offset + STOP + 再读"
 *   （那样芯片内部地址指针行为不保证）。
 * · 连续读多字节：地址自动加一，手册允许一个事务读完。
 * =========================================================================== */

static esp_err_t i2c_wr_reg(uint8_t reg, uint8_t val)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < I2C_TRIES; i++) {
        err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    s_i2c_err++;
    return err;
}

static esp_err_t i2c_wr_block(uint8_t reg, const uint8_t *data, size_t n)
{
    if (s_dev == NULL || data == NULL || n == 0 || n > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[9];
    buf[0] = reg;
    memcpy(&buf[1], data, n);

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < I2C_TRIES; i++) {
        err = i2c_master_transmit(s_dev, buf, n + 1, I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    s_i2c_err++;
    return err;
}

static esp_err_t i2c_rd_reg(uint8_t reg, uint8_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < I2C_TRIES; i++) {
        err = i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    s_i2c_err++;
    return err;
}

static esp_err_t i2c_rd_block(uint8_t reg, uint8_t *out, size_t n)
{
    if (s_dev == NULL || out == NULL || n == 0 || n > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < I2C_TRIES; i++) {
        err = i2c_master_transmit_receive(s_dev, &reg, 1, out, n, I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    s_i2c_err++;
    return err;
}

static bool i2c_probe(void)
{
    if (s_bus == NULL) {
        return false;
    }
    for (int i = 0; i < I2C_TRIES; i++) {
        if (i2c_master_probe(s_bus, EDX106_I2C_ADDR_7BIT, I2C_XFER_TIMEOUT_MS) == ESP_OK) {
            return true;
        }
    }
    return false;
}

static void bus_close(void)
{
    if (s_dev != NULL) {
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus != NULL) {
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

/**
 * 建 I2C 主机总线（含 0x28 从机句柄）。
 *
 * ⚠ **上拉要点（2026-10-26 用户变更：样机全程用内部上拉）**：
 *   `enable_internal_pullup = true` 是 SDA/SCL **唯一**的上拉来源。板上的
 *   R13/R14（4.7 kΩ 上拉位）是 DNP，而且挂在 IO6/IO7 网络上 —— 与本次用的
 *   IO4/IO5 无关，帮不上忙。C6 内部上拉约 45 kΩ 量级，配上跳线电容后上升沿
 *   很慢，所以默认频率取 **100 kHz**，并在 probe 失败时逐级降到 50 kHz。
 *   量产建议：H3 侧外置 2×4.7 kΩ，再把频率升回 400 kHz（芯片上限）。
 */
static esp_err_t bus_open(uint32_t freq_hz)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                 /* 自动选端口（C6 上只有 I2C0 可用作主机） */
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags.enable_internal_pullup = true,   /* ★ SDA/SCL 唯一的上拉来源（见下） */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EDX106_I2C_ADDR_7BIT,
        .scl_speed_hz = freq_hz,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }
    s_i2c_freq_hz = freq_hz;
    return ESP_OK;
}

/**
 * 在指定频率上"开总线 → 挂 0x28 → probe"这一整步。
 * 返回值语义分三类，调用方要据此决定"降频能不能救"：
 *   ESP_OK              成功（器件应答了）
 *   ESP_ERR_NOT_FOUND   总线正常但器件没应答（→ 值得降频再试）
 *   其它                总线/驱动层面失败（→ 降频救不了，直接报错）
 */
static esp_err_t i2c_try_open_at(uint32_t freq_hz)
{
    bus_close();
    esp_err_t err = bus_open(freq_hz);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_probe() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ===========================================================================
 * 6. 小工具
 * =========================================================================== */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static bool out_level_is_active(int level)
{
    return (level != 0) == (CFG_OUT_ACTIVE_HIGH != 0);
}

/* ===========================================================================
 * 7. 橙灯（IO15，高电平点亮）—— 三种状态 + 4 条手动诊断命令
 * ---------------------------------------------------------------------------
 * 用户拍板（2026-10-18 补充）：「现在只要实现触发后就亮灯即可」。所以橙灯是本 demo
 * 唯一的**人可感知**输出，三种状态必须分得清、且都能 5 秒内自证：
 *
 *   ① 跟随 OUT（默认，CFG_LED_FOLLOW_OUT=1）
 *        灯 = OUT 电平。亮多久由**芯片的 delay**（0x46~0x49）决定，MCU 不做定时。
 *        实测：trig=1 → trig=0 width=2360 ⇒ 灯亮约 2.4 s。
 *   ② 闪一下（CFG_LED_FOLLOW_OUT=0）
 *        上升沿点亮 → LED_FLASH_MS(300 ms) 后由 monitor 熄灭（时间戳判定）。
 *        适合"只要有个'有动'的提示，不想灯一直亮"的场合。
 *   ③ 手动（`LED on` / `LED off` / `LED blink`）
 *        **只动 IO15**：不碰 I2C、不影响 OUT 中断、不改任何寄存器。
 *        手动点亮之后再来一次触发，上升沿仍会把灯置"亮"（幂等），下降沿仍会熄灭 ——
 *        手动与触发不互斥、也不冲突（led_set 是纯电平写，没有状态机）。
 *
 * 为什么要做手动命令：现场"灯不亮"是最高频的争议点，而它的原因可能有三层
 * （GPIO 没生效 / 灯具或限流电阻坏了 / 根本没触发）。有了 `LED on` + `LED?` 的
 * level 回读，就能把三层一刀切开 —— 排查顺序写进 notes §15.3。
 * =========================================================================== */

static esp_err_t led_gpio_init(void)
{
#if CFG_LED_ENABLE
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (int)PIN_LED,
        /* 用 INPUT_OUTPUT 而不是纯 OUTPUT：本工程踩过「纯 OUTPUT 时
         * gpio_get_level() 在 C6 上恒返回 0」的坑（docs/EDK60-pitfalls.md C3），
         * 配成 INPUT_OUTPUT 才能可靠回读实际电平（诊断时很有用）。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* 初始灭（高电平点亮 ⇒ 灭 = 低电平） */
    return gpio_set_level(PIN_LED, !EDK60_RADAR_LED_ACTIVE_LEVEL);
#else
    return ESP_OK;
#endif
}

static void led_set(bool on)
{
#if CFG_LED_ENABLE
    (void)gpio_set_level(PIN_LED,
                         on ? EDK60_RADAR_LED_ACTIVE_LEVEL : !EDK60_RADAR_LED_ACTIVE_LEVEL);
#else
    (void)on;   /* Kconfig 关掉橙灯：只上报，不点灯 */
#endif
}

/**
 * 回读 IO15 的**实测电平**（诊断的唯一事实来源）。
 * 依赖 `GPIO_MODE_INPUT_OUTPUT`（纯 OUTPUT 时 C6 上 `gpio_get_level()` 恒返回 0，
 * 见 docs/EDK60-pitfalls.md C3）。Kconfig 关掉橙灯时返回 -1（"没这根线"）。
 */
static int led_read_level(void)
{
#if CFG_LED_ENABLE
    return gpio_get_level(PIN_LED);
#else
    return -1;
#endif
}

/** 电平 → "是否点亮"（高电平点亮时 level=1 才是亮；写成比较式，不写死 1） */
static int led_level_is_on(int level)
{
    if (level < 0) {
        return 0;
    }
    return ((level != 0) == (EDK60_RADAR_LED_ACTIVE_LEVEL != 0)) ? 1 : 0;
}

/** `LED on` / `LED off`：只写 IO15，并把**实测读回电平**打出来（不是我们期望的值）。 */
static void cmd_led_manual(bool on)
{
#if CFG_LED_ENABLE
    /* 手动操作取消未完成的"闪一下"窗口：否则手动点亮 200 ms 后会被旧窗口熄灭 */
    s_led_flash_off_us = 0;
    s_led_by_trigger = false;
    led_set(on);
    usb_line("EDX106: led %s gpio=%d level=%d", on ? "on" : "off", (int)PIN_LED,
             led_read_level());
#else
    (void)on;
    usb_line("EDX106: led disabled (CONFIG_APP_EDK60_EDX106_LED_ENABLE=n)");
#endif
}

/** `LED?`：一次性给出判断"灯到底亮不亮"所需的全部事实（含 Kconfig 状态）。 */
static void cmd_led_status(void)
{
    int level = led_read_level();
    usb_line("EDX106: led? level=%d on=%d active_level=%d cfg_enable=%d trigger_led=%s "
             "follow_out=%d",
             level, led_level_is_on(level), EDK60_RADAR_LED_ACTIVE_LEVEL, CFG_LED_ENABLE,
             s_led_by_trigger ? "true" : "false", CFG_LED_FOLLOW_OUT);
}

/** `LED blink`：亮灭 3 次 × 200 ms —— "5 秒内确认灯能不能亮"的一条命令。
 *  阻塞 1.2 s 在 task_cmd 里执行：这段时间不持有任何 I2C/队列资源，
 *  monitor 任务照常跑（事件照常上报），所以阻塞是可以接受的。 */
static void cmd_led_blink(void)
{
#if CFG_LED_ENABLE
    s_led_flash_off_us = 0;
    s_led_by_trigger = false;
    for (int i = 0; i < LED_BLINK_TIMES; i++) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_MS));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_MS));
    }
    usb_line("EDX106: led blink done");
#else
    usb_line("EDX106: led disabled (CONFIG_APP_EDK60_EDX106_LED_ENABLE=n)");
#endif
}

/* ===========================================================================
 * 8. 配置块：写入 + 回读校验
 * =========================================================================== */

/**
 * 写配置块。**顺序有讲究**：0x6e（bit6=1 阈值走寄存器）必须排在 0x0f/0x10 之前，
 * 否则"先写了阈值、再打开开关"在某些时序下会让第一次判决用旧源；
 * 反过来（先开开关再写阈值）在任何理解下都正确。
 */
static esp_err_t apply_config(void)
{
    uint8_t b[4];
    esp_err_t err;

    /* 0x6c：ULP + 模式走寄存器 + 关光敏（兼容保留）+ bits1:0=0b11 */
    err = i2c_wr_reg(REG_CNF_1, REG_6C_VALUE);
    if (err != ESP_OK) {
        return err;
    }
    /* 0x6d：参数可调 + 最低功耗档 */
    err = i2c_wr_reg(REG_CNF_WORK_TIME, REG_6D_VALUE);
    if (err != ESP_OK) {
        return err;
    }
    /* 0x6e：**阈值走寄存器**（这一步不做，下面的阈值写了也不生效） */
    err = i2c_wr_reg(REG_CNF_BURST_THR, REG_6E_VALUE);
    if (err != ESP_OK) {
        return err;
    }
    /* 阈值 0~1023：低 8 位 + 高 2 位 */
    err = i2c_wr_reg(REG_THR_L, (uint8_t)(s_thr & 0xFF));
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_wr_reg(REG_THR_H, (uint8_t)((s_thr >> 8) & 0x03));
    if (err != ESP_OK) {
        return err;
    }
    /* 0x45 bit0=1：**延时走寄存器**（模组没引出 delay pin） */
    err = i2c_wr_reg(REG_DELAY_SEL, REG_DELAY_SEL_VALUE);
    if (err != ESP_OK) {
        return err;
    }
    /* 0x46~0x49：延时 32 位小端，单位 ms */
    put_le32(b, (uint32_t)s_delay_ms);
    err = i2c_wr_block(REG_DELAY0, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    /* 0x52/0x53：灭灯保护（闭锁）时间，单位 ms（只有 16 位） */
    err = i2c_wr_reg(REG_LOCKOUT_L, (uint8_t)(s_lockout_ms & 0xFF));
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_wr_reg(REG_LOCKOUT_H, (uint8_t)((s_lockout_ms >> 8) & 0xFF));
    if (err != ESP_OK) {
        return err;
    }
    /* 0x60：第一级接收增益（默认值，不轻易改） */
    return i2c_wr_reg(REG_GAIN1, REG_GAIN1_VALUE);
}

/**
 * 回读校验：把写过的 13 个寄存器逐个读回并逐字节比对。
 * 返回不一致的个数（0 = 全通过）。
 * 不一致时**逐条打印** `CFG_MISMATCH reg=0xNN wrote=0xNN read=0xNN` ——
 * 这是现场判断"到底哪一位没写进去"的唯一线索（尤其 0x6e bit6）。
 */
static int verify_config(void)
{
    uint8_t d[4];
    put_le32(d, (uint32_t)s_delay_ms);

    const uint8_t exp_reg[13] = {
        REG_CNF_1, REG_CNF_WORK_TIME, REG_CNF_BURST_THR,
        REG_THR_L, REG_THR_H,
        REG_DELAY_SEL,
        REG_DELAY0, REG_DELAY1, REG_DELAY2, REG_DELAY3,
        REG_LOCKOUT_L, REG_LOCKOUT_H,
        REG_GAIN1,
    };
    const uint8_t exp_val[13] = {
        REG_6C_VALUE, REG_6D_VALUE, REG_6E_VALUE,
        (uint8_t)(s_thr & 0xFF), (uint8_t)((s_thr >> 8) & 0x03),
        REG_DELAY_SEL_VALUE,
        d[0], d[1], d[2], d[3],
        (uint8_t)(s_lockout_ms & 0xFF), (uint8_t)((s_lockout_ms >> 8) & 0xFF),
        REG_GAIN1_VALUE,
    };

    int mism = 0;
    for (size_t i = 0; i < sizeof(exp_reg) / sizeof(exp_reg[0]); i++) {
        uint8_t rd = 0;
        if (i2c_rd_reg(exp_reg[i], &rd) != ESP_OK) {
            usb_line("EDX106: CFG_MISMATCH reg=0x%02x wrote=0x%02x read=ERR",
                     exp_reg[i], exp_val[i]);
            mism++;
            continue;
        }
        if (rd != exp_val[i]) {
            usb_line("EDX106: CFG_MISMATCH reg=0x%02x wrote=0x%02x read=0x%02x",
                     exp_reg[i], exp_val[i], rd);
            mism++;
        }
    }
    return mism;
}

/* ===========================================================================
 * 8.1 运行参数回读（一行 run ... 供现场判定"改的参数到底生效没有"）
 * =========================================================================== */

/**
 * 打印**芯片里实际生效的**运行参数（不是 Kconfig 值，也不是我们写的期望值）。
 *
 * 一行定现场争议：`EDX106: run mode=<ulp|normal> thr=<n> delay=<ms> lockout=<ms>
 * gain=0xNN(0x60) win=<len>/<total>/<cnt>`
 *   · mode  ← 0x6c bit7 回读（`SET mode=` 之后以它为准）
 *   · thr   ← 0x0f/0x10 回读拼装
 *   · delay ← 0x46~0x49 回读
 *   · lockout ← 0x52/0x53 回读
 *   · gain  ← 0x60 回读（3 dB/step，**值越大增益越小**）
 *   · win   ← 0x0d / 0x30 / 0x31 回读（窗口长度 / 观察窗口数 / 需超阈值的窗口数）
 *
 * 调用点：bring-up 末尾（含 RESET 命令）与每条成功的 `SET` 之后 —— 这样"改了参数到底
 * 生效没有"永远不需要靠记忆或另开一次 `CFG?` 去猜。
 */
static void report_run_state(void)
{
    if (s_bus == NULL || s_dev == NULL) {
        return;   /* 诊断模式（I2C 都没起来）：没有可回读的寄存器，静默返回 */
    }

    uint8_t v6c = 0, vl = 0, vh = 0, g1 = 0;
    uint8_t wlen = 0, wtot = 0, wcnt = 0;
    uint8_t dly[4] = { 0 };
    uint8_t lk[2] = { 0 };

    (void)i2c_rd_reg(REG_CNF_1, &v6c);
    (void)i2c_rd_reg(REG_THR_L, &vl);
    (void)i2c_rd_reg(REG_THR_H, &vh);
    (void)i2c_rd_block(REG_DELAY0, dly, sizeof(dly));
    (void)i2c_rd_block(REG_LOCKOUT_L, lk, sizeof(lk));
    (void)i2c_rd_reg(REG_GAIN1, &g1);
    (void)i2c_rd_reg(REG_WIN_LEN, &wlen);
    (void)i2c_rd_reg(REG_WIN_TOTAL, &wtot);
    (void)i2c_rd_reg(REG_WIN_CNT, &wcnt);

    /* 让 trig=1 / cfg ok 两行的 mode= 字段跟着**实际模式**走（字段名不变） */
    s_mode_ulp = ((v6c >> 7) & 1) != 0;

    usb_line("EDX106: run mode=%s thr=%u delay=%u lockout=%u gain=0x%02x(0x60) "
             "win=%u/%u/%u",
             s_mode_ulp ? "ulp" : "normal",
             (unsigned)(((uint32_t)(vh & 0x03) << 8) | vl),
             (unsigned)get_le32(dly),
             (unsigned)((uint32_t)lk[0] | ((uint32_t)lk[1] << 8)),
             g1, (unsigned)wlen, (unsigned)wtot, (unsigned)wcnt);
}

/* ===========================================================================
 * 9. 启动序列
 * =========================================================================== */

/**
 * @param power_cycle true = 真的给模组上下电（首次启动）；
 *                    false = 只做软复位（RESET 命令，避免反复热插拔式掉电）
 * @return ESP_OK = 走到"可以开监控"的程度；其它 = 停在诊断模式
 */
static esp_err_t bringup_sequence(bool power_cycle)
{
    s_cfg_verified = false;

    /* --- 1. 雷达电源：IO14 拉高，等 ≥100 ms --- */
    if (power_cycle) {
        esp_err_t err = edk60_radar_power_init();
        if (err != ESP_OK) {
            usb_line("EDX106: err PWR_INIT gpio=%d err=%s", (int)PIN_PWR_EN,
                     esp_err_to_name(err));
            return err;
        }
        err = edk60_radar_power_on();
        if (err != ESP_OK) {
            usb_line("EDX106: err PWR_INIT gpio=%d err=%s", (int)PIN_PWR_EN,
                     esp_err_to_name(err));
            return err;
        }
    } else if (!edk60_radar_power_is_on()) {
        esp_err_t err = edk60_radar_power_on();
        if (err != ESP_OK) {
            usb_line("EDX106: err PWR_INIT gpio=%d err=%s", (int)PIN_PWR_EN,
                     esp_err_to_name(err));
            return err;
        }
    }

    /* 自检窗口从"模组上电/软复位那一刻"起算：手册说上电自检期间 OUT 强制高
     * 约 3000 ms（0x3c/0x3d）。这段时间内的任何"高电平"都不是有人，必须丢弃。 */
    s_selfcheck_until_us = now_us() + (int64_t)CONFIG_APP_EDK60_EDX106_SELFCHECK_MS * 1000;
    s_selfcheck_dropped = 0;

    usb_line("EDX106: boot pwr_en=%d gpio=%d settle=%dms selfcheck=%dms",
             (int)edk60_radar_power_is_on(), (int)PIN_PWR_EN,
             RADAR_PWR_SETTLE_MS, CONFIG_APP_EDK60_EDX106_SELFCHECK_MS);
    vTaskDelay(pdMS_TO_TICKS(RADAR_PWR_SETTLE_MS));

    /* --- 2. I2C 总线 + 3. probe（失败 → 逐级降频重试：配置值 → 100k → 50k） ---
     * 为什么要降频：板上 R13/R14（4.7 kΩ 上拉位）是 DNP，且挂在 IO6/IO7 上与本次
     * 无关；样机阶段用户也没有外部电阻 ⇒ 只有 **C6 内部上拉（~45 kΩ 量级）**。
     * 45 kΩ × 总线电容会给出很慢的上升沿：频率越高越容易 ACK 收不到。
     * 降频不改变"到底有没有器件"这个事实，却能避免把上拉不足误判成接线错。 */
    bus_close();
    uint32_t tries[3];
    int ntries = 0;
    uint32_t cfg_freq = (uint32_t)CONFIG_APP_EDK60_EDX106_I2C_FREQ_HZ;
    tries[ntries++] = cfg_freq;
    if (cfg_freq > (uint32_t)I2C_FREQ_STEP1_HZ) {
        tries[ntries++] = (uint32_t)I2C_FREQ_STEP1_HZ;
    }
    if (tries[ntries - 1] > (uint32_t)I2C_FREQ_STEP2_HZ) {
        tries[ntries++] = (uint32_t)I2C_FREQ_STEP2_HZ;
    }

    bool probed = false;
    uint32_t freq = cfg_freq;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < ntries; i++) {
        err = i2c_try_open_at(tries[i]);
        if (err == ESP_OK) {
            probed = true;
            freq = tries[i];
            break;
        }
        if (err == ESP_ERR_NOT_FOUND) {
            usb_line("EDX106: probe addr=0x28 fail freq=%u", (unsigned)tries[i]);
        } else {
            /* 总线都建不起来是另一类错误（引脚被占/驱动装不上），降频救不了 */
            usb_line("EDX106: err I2C_BUS_INIT freq=%u scl=%d sda=%d err=%s",
                     (unsigned)tries[i], (int)PIN_SCL, (int)PIN_SDA,
                     esp_err_to_name(err));
            return err;
        }
        if ((i + 1) < ntries) {
            usb_line("EDX106: I2C_DOWNSHIFT %u -> %u (retry)", (unsigned)tries[i],
                     (unsigned)tries[i + 1]);
        }
    }

    if (!probed) {
        /* 停在这里不重启：让用户能继续用 SCAN 排查接线，改好后发 RESET 即可。
         * 现象与对策的对应关系（现场最常踩的三条）：
         *   ① 内部上拉不够（长线/多器件）→ 各级频率都失败 → 需要 2×4.7 kΩ 外置；
         *   ② SCL/SDA 接反               → 一直失败；
         *   ③ IO14 没拉高（3V3_RADAR 没电）→ 一直失败（但上一行已打印 pwr_en=1）。 */
        usb_line("EDX106: err I2C_NO_DEVICE tried=%d lowest_freq=%u scl=%d sda=%d",
                 ntries, (unsigned)tries[ntries - 1], (int)PIN_SCL, (int)PIN_SDA);
        /* 只提示、不阻塞：样机阶段用户没有电阻，能做的就是接线与降频 */
        usb_line("EDX106: hint 连续 %d 次 probe 均失败 → 建议加 2x4.7k 外置上拉"
                 "（内部上拉仅供调试）", ntries);
        return ESP_ERR_NOT_FOUND;
    }
    /* 把**实际使用的频率**打出来：现场一眼就能判断是不是靠降频才通的 */
    usb_line("EDX106: i2c ok freq=%u scl=%d sda=%d addr=0x28 pullup=internal%s",
             (unsigned)freq, (int)PIN_SCL, (int)PIN_SDA,
             (freq < cfg_freq) ? " (已降频)" : "");

    /* --- 4. 存在性探针：读 0x0d，期望 0x0a（不一致只记录，不中止） --- */
    uint8_t id = 0;
    err = i2c_rd_reg(REG_ID_PROBE, &id);
    if (err != ESP_OK) {
        usb_line("EDX106: err ID_READ reg=0x%02x err=%s", REG_ID_PROBE,
                 esp_err_to_name(err));
        return err;
    }
    usb_line("EDX106: id reg0x%02x=0x%02x expect=0x%02x%s", REG_ID_PROBE, id,
             REG_ID_PROBE_EXPECT,
             (id == REG_ID_PROBE_EXPECT) ? "" : " (mismatch, continue)");

    /* --- 5. 软复位：0x00=0x00 → 10 ms → 0x00=0x01 → 20 ms --- */
    err = i2c_wr_reg(REG_RSTN_SW, 0x00);
    if (err != ESP_OK) {
        usb_line("EDX106: err CFG_WRITE reg=0x%02x err=%s", REG_RSTN_SW,
                 esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SOFT_RESET_HOLD_MS));
    err = i2c_wr_reg(REG_RSTN_SW, 0x01);
    if (err != ESP_OK) {
        usb_line("EDX106: err CFG_WRITE reg=0x%02x err=%s", REG_RSTN_SW,
                 esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SOFT_RESET_RELEASE_MS));
    usb_line("EDX106: reset 0x00=0x00 ->%dms-> 0x00=0x01 ->%dms", SOFT_RESET_HOLD_MS,
             SOFT_RESET_RELEASE_MS);

    /* --- 6. 写配置块 --- */
    err = apply_config();
    if (err != ESP_OK) {
        usb_line("EDX106: err CFG_WRITE block err=%s", esp_err_to_name(err));
        return err;
    }

    /* --- 7. 回读校验（13 个寄存器逐字节） --- */
    int mism = verify_config();

    /* 三个"缺一不可"的控制寄存器，把**回读实际值**单独打一行：
     * 现场只要看这一行就知道"模式/阈值/延时到底走的是寄存器还是（模组内部）pin"。 */
    uint8_t v6c = 0, v6d = 0, v6e = 0, v45 = 0;
    (void)i2c_rd_reg(REG_CNF_1, &v6c);
    (void)i2c_rd_reg(REG_CNF_WORK_TIME, &v6d);
    (void)i2c_rd_reg(REG_CNF_BURST_THR, &v6e);
    (void)i2c_rd_reg(REG_DELAY_SEL, &v45);
    usb_line("EDX106: ctrl 0x6c=0x%02x(mode_reg=%d ulp=%d light_off=%d) "
             "0x6d=0x%02x(pwr_step=%d) 0x6e=0x%02x(thr_reg=%d burst_reg=%d) "
             "0x45=0x%02x(delay_reg=%d)",
             v6c, (v6c >> 6) & 1, (v6c >> 7) & 1, (v6c >> 4) & 1,
             v6d, v6d & 0x07,
             v6e, (v6e >> 6) & 1, (v6e >> 5) & 1,
             v45, v45 & 1);

    /* 把"实际模式"记进运行期状态：cfg ok / trig=1 两行的 mode= 用它（字段不变，取值更诚实） */
    s_mode_ulp = ((v6c >> 7) & 1) != 0;

    if (mism == 0) {
        s_cfg_verified = true;
        usb_line("EDX106: cfg ok thr=%u delay=%u lockout=%u mode=%s light=%s",
                 (unsigned)s_thr, (unsigned)s_delay_ms, (unsigned)s_lockout_ms,
                 s_mode_ulp ? "ulp" : "normal", CFG_LIGHT_OFF ? "off" : "on");
    } else {
        /* 回读不一致**不是**致命错误：OUT 仍然会动（芯片用它的默认/残留配置在工作），
         * 但阈值/延时可能完全不是我们要的。所以继续进监控，同时把状态说清楚。 */
        usb_line("EDX106: cfg fail mismatches=%d（继续监控，但参数可能不生效）", mism);
    }

    /* 一行交代"芯片里实际生效的参数"（回读值，不是 Kconfig 值）。RESET 命令也会走到这里。 */
    report_run_state();
    return ESP_OK;
}

/* ===========================================================================
 * 10. OUT 输入 + 双边沿中断
 * =========================================================================== */

/**
 * ISR：**只做两件事** —— 读一次电平、投递到一个队列。
 * 严禁在 ISR 里做 I2C、打印、分配内存（本项目硬规则；in-I2C-from-ISR 会直接死锁）。
 */
static void IRAM_ATTR out_edge_isr(void *arg)
{
    (void)arg;
    uint32_t level = (uint32_t)gpio_get_level(PIN_OUT);
    BaseType_t woken = pdFALSE;
    if (s_out_q != NULL) {
        (void)xQueueSendFromISR(s_out_q, &level, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * 只做**引脚配置**：输入 + 内部上拉，永不驱动。**必须在模组上电（IO14 拉高）之前**调用。
 *
 * 为什么顺序要提前（2026-10-26 用户变更）：
 *   OUT 是模组输出，而模组由 3V3_RADAR 供电、由 IO14 使能 —— 在固件拉高 IO14 之前，
 *   模组**没有电**，OUT 是**高阻**。如果这时 IO2 上没有任何上拉，引脚就是浮空的，
 *   读回来是随机值，还可能被 strapping 采样到不确定电平。
 *   ⇒ 先把 IO2 配成「输入 + 内部上拉」（`GPIO_PULLUP_ONLY` 语义：只上拉、不下拉、
 *     不驱动），再给模组上电。这样从第一刻起该脚都有确定电平（= 高 = 无触发）。
 *
 * ⚠ 风险与 Plan B（必须知道）：
 *   IO2 是 **C6 的 strapping 关注脚**。app 运行前（boot 阶段）该脚电平由芯片内部
 *   默认 + 外部电路决定，**本固件无法介入**。若现场出现"上电偶发进下载模式/启动异常"，
 *   **Plan B = 把 OUT 改到 H3 的 `A3`（IO3）**：同属 GPIO0~7（EXT1 唤醒域可用）、
 *   **不是 strapping 脚**，而且板上没有任何功能网络占用它。
 *   改法只有两步：Kconfig 的 `OUT_GPIO` 改成 3 + 把模组 OUT 线挪到 H3 的 A3。
 */
static esp_err_t out_gpio_config_input(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (int)PIN_OUT,
        /* 只配 INPUT：**本固件在任何时刻都不驱动这根脚**（mod 输出推挽 3.3V±0.1V）。
         * 内部上拉给"模组未上电时的高阻态"一个确定电平，也顺带覆盖"模组没插"的情况。 */
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,   /* 绝不能下拉：IO2 拉低会影响启动模式 */
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    usb_line("EDX106: out gpio=%d mode=input pull=up active_high=%d (未驱动，先于模组上电配置)",
             (int)PIN_OUT, CFG_OUT_ACTIVE_HIGH);
    return ESP_OK;
}

/**
 * 挂中断（启动序列第 8 步，配置块写完并回读校验之后）。
 * 幂等：重复调用只重新 `gpio_intr_enable`，不会重复注册 ISR handler。
 */
static esp_err_t out_gpio_attach_isr(void)
{
    if (!s_out_armed) {
        esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
            return isr_err;
        }
        esp_err_t err = gpio_isr_handler_add(PIN_OUT, out_edge_isr, NULL);
        if (err != ESP_OK) {
            return err;
        }
        s_out_armed = true;
    }
    esp_err_t err = gpio_intr_enable(PIN_OUT);
    if (err != ESP_OK) {
        return err;
    }
    usb_line("EDX106: out irq=anyedge armed gpio=%d", (int)PIN_OUT);
    return ESP_OK;
}

/* ===========================================================================
 * 11. 监控任务：电平判定（以轮询为准）+ 橙灯 + 事件上报 + 周期统计
 * =========================================================================== */

/** 一次真实的电平跳变（两种来源都汇到这里：ISR 队列 / 轮询发现） */
static void handle_transition(int level, int64_t t_us)
{
    int64_t t_ms = t_us / 1000;
    bool active = out_level_is_active(level);

    if (active) {
        s_last_rise_us = t_us;
        s_trig_total++;
        s_stat_trig++;
        /* 触发 → 亮灯（用户 2026-10-18 需求："触发后就亮灯"）。
         * 手动 `LED on` 之后再来一次触发，这一句仍是幂等的（led_set 是纯电平写）。 */
        led_set(true);
        s_led_by_trigger = true;
#if CFG_LED_FOLLOW_OUT
        /* 跟随 OUT：亮多久由芯片 delay 决定，MCU 不设超时 */
        s_led_flash_off_us = 0;
#else
        /* 闪一下：到点由 monitor 熄灭（时间戳判定，不用定时器中断） */
        s_led_flash_off_us = t_us + (int64_t)LED_FLASH_MS * 1000;
#endif
        usb_line("EDX106: t=%" PRId64 " trig=1 level=%d delay=%u lockout=%u thr=%u mode=%s",
                 t_ms, level, (unsigned)s_delay_ms, (unsigned)s_lockout_ms,
                 (unsigned)s_thr, s_mode_ulp ? "ulp" : "normal");
        /* 新增独立行：把"灯已被触发点亮"显式留痕。
         * 注意：上面 trig=1 那行的**字段一个没动**（上游解析器依赖），新信息只在这行。 */
        usb_line("EDX106: led on (trigger)");
    } else {
        /* 高电平宽度：用"这次下降沿时刻 - 上次上升沿时刻"实测，
         * **不用配置值代替实测值** —— 配置值只是期望，实测值才能暴露芯片没按
         * 我们写的 delay 工作（例如 0x45 bit0 没写进去时用的是内部 pin 的档位）。 */
        int64_t span_us = (s_last_rise_us > 0) ? (t_us - s_last_rise_us) : 0;
        uint32_t width = (span_us > 0) ? (uint32_t)(span_us / 1000) : 0;
        led_set(false);
        s_led_flash_off_us = 0;          /* 下降沿已经熄灭，撤掉未完成的闪灯窗口 */
        s_led_by_trigger = false;
        s_last_width_ms = width;
        if (width < s_stat_wmin) {
            s_stat_wmin = width;
        }
        if (width > s_stat_wmax) {
            s_stat_wmax = width;
        }
        usb_line("EDX106: t=%" PRId64 " trig=0 width=%u", t_ms, (unsigned)width);
        usb_line("EDX106: led off (release)");
    }
}

static void stats_tick(int64_t t_us)
{
    if (CONFIG_APP_EDK60_EDX106_STATS_SEC <= 0) {
        return;   /* Kconfig 允许关掉周期统计 */
    }
    if (t_us < s_next_stat_us) {
        return;
    }
    s_next_stat_us = t_us + (int64_t)CONFIG_APP_EDK60_EDX106_STATS_SEC * 1000000;

    usb_line("EDX106: stat interval=%d trig=%u width_min=%u width_max=%u i2c_err=%u",
             CONFIG_APP_EDK60_EDX106_STATS_SEC, (unsigned)s_stat_trig,
             (unsigned)((s_stat_wmin == UINT32_MAX) ? 0 : s_stat_wmin),
             (unsigned)s_stat_wmax, (unsigned)s_i2c_err);

    s_stat_trig = 0;
    s_stat_wmin = UINT32_MAX;
    s_stat_wmax = 0;
}

static void task_monitor(void *arg)
{
    (void)arg;

    int last_level = gpio_get_level(PIN_OUT);
    int64_t sc_deadline = s_selfcheck_until_us;
    bool sc_reported = false;

    while (1) {
        uint32_t queued = 0;
        /* 20 ms 节拍：有中断时立刻醒（及时性），没中断时也定时看一眼（防丢沿） */
        bool got_edge = (xQueueReceive(s_out_q, &queued, pdMS_TO_TICKS(MON_POLL_MS)) == pdTRUE);

        int64_t t = now_us();
        int level = gpio_get_level(PIN_OUT);   /* **电平永远是权威** */

        /* --- 自检窗口：窗口内丢弃事件（第一次上电 / 每次 RESET 后重开） --- */
        if (s_selfcheck_until_us != sc_deadline) {
            sc_deadline = s_selfcheck_until_us;
            sc_reported = false;
        }
        if (t < sc_deadline) {
            if (level != last_level) {
                last_level = level;
                s_selfcheck_dropped++;
            }
            led_set(false);          /* 自检期不亮橙灯，避免误以为"有人" */
            s_led_flash_off_us = 0;  /* 顺便撤掉 RESET 前遗留的闪灯窗口 */
            continue;
        }
        if (!sc_reported) {
            sc_reported = true;
            last_level = level;      /* 以窗口结束瞬间的实际电平作为基线 */
            /* 边界情况：窗口结束的那一刻 OUT 仍然是高（模组自检比我们的窗口长，
             * 或窗口内刚好有人）。此时把"窗口结束"当作上升沿，否则后面那次下降沿
             * 会拿一个陈旧的上升沿去算宽度，打出一个荒唐的 width 值。 */
            if (out_level_is_active(level)) {
                s_last_rise_us = t;
            }
#if CFG_LED_FOLLOW_OUT
            led_set(out_level_is_active(level));
#else
            /* 闪一下模式：自检窗口结束时的电平**不是一次触发**（没有 trig=1 事件），
             * 所以不点灯 —— 否则会出现"灯亮着但日志里没有任何事件"的诡异状态。 */
            led_set(false);
            s_led_flash_off_us = 0;
#endif
            usb_line("EDX106: SELFCHECK done level=%d dropped=%u", level,
                     (unsigned)s_selfcheck_dropped);
        } else if (level != last_level) {
            /* 中断没来（丢沿/抖动）但电平变了 → **以轮询电平为准**并记录纠偏次数 */
            if (!got_edge) {
                s_poll_resync++;
            }
            handle_transition(level, t);
            last_level = level;
        }

        /* --- `LED_FOLLOW_OUT=n`（闪一下）：到点熄灭 ---
         * 时间戳判定，20 ms 节拍的精度对"给个提示"完全够；**不用定时器/定时器中断**，
         * 因为那会在中断上下文里碰 GPIO，还会和手动 LED 命令抢状态。
         * FOLLOW_OUT=y 时 s_led_flash_off_us 恒为 0 ⇒ 这一段是空转，行为与旧版完全一致。 */
        if (s_led_flash_off_us != 0 && t >= s_led_flash_off_us) {
            s_led_flash_off_us = 0;
            led_set(false);
            s_led_by_trigger = false;
            usb_line("EDX106: led off (flash %dms)", LED_FLASH_MS);
        }

        stats_tick(t);
    }
}

/* ===========================================================================
 * 12. 诊断命令
 * =========================================================================== */

static void cmd_help(void)
{
    usb_line("EDX106: help CFG? | SCAN | TRIG? | LED on|off|blink | LED? | "
             "SET delay=<ms>|thr=<0..1023>|lockout=<ms>|mode=ulp|normal|gain=<0..15>|"
             "win=<len>,<total>,<cnt> | RESET");
}

static bool i2c_ready(void)
{
    return (s_bus != NULL) && (s_dev != NULL);
}

/** CFG? —— 逐条回读并翻译成语义，不缓存、不猜。 */
static void cmd_dump_cfg(void)
{
    if (!i2c_ready()) {
        usb_line("EDX106: err NOT_READY code=NOT_CONFIGURED hint=\"先 RESET 走完启动序列\"");
        return;
    }

    uint8_t v6c = 0, v6d = 0, v6e = 0, v45 = 0, vl = 0, vh = 0;
    uint8_t dly[4] = { 0 }, lk[2] = { 0 }, g1 = 0;

    (void)i2c_rd_reg(REG_CNF_1, &v6c);
    (void)i2c_rd_reg(REG_CNF_WORK_TIME, &v6d);
    (void)i2c_rd_reg(REG_CNF_BURST_THR, &v6e);
    (void)i2c_rd_reg(REG_THR_L, &vl);
    (void)i2c_rd_reg(REG_THR_H, &vh);
    (void)i2c_rd_reg(REG_DELAY_SEL, &v45);
    (void)i2c_rd_block(REG_DELAY0, dly, sizeof(dly));
    (void)i2c_rd_block(REG_LOCKOUT_L, lk, sizeof(lk));
    (void)i2c_rd_reg(REG_GAIN1, &g1);

    usb_line("EDX106: cfg state verified=%d（1 = 启动时 13 个寄存器回读全通过）",
             s_cfg_verified ? 1 : 0);
    usb_line("EDX106: reg 0x6c=0x%02x mode=%s mode_reg=%d ulp=%d light_off=%d",
             v6c, ((v6c >> 6) & 1) ? "reg" : "pin", (v6c >> 6) & 1, (v6c >> 7) & 1,
             (v6c >> 4) & 1);
    usb_line("EDX106: reg 0x6d=0x%02x adj=%d pwr_step=%d", v6d, (v6d >> 3) & 1, v6d & 0x07);
    usb_line("EDX106: reg 0x6e=0x%02x thr_src=%s thr_reg=%d burst_reg=%d burst_4_3=%d",
             v6e, ((v6e >> 6) & 1) ? "reg" : "pin", (v6e >> 6) & 1, (v6e >> 5) & 1,
             (v6e >> 3) & 0x03);
    usb_line("EDX106: reg 0x0f=0x%02x 0x10=0x%02x thr=%u%s", vl, vh,
             (unsigned)(((uint32_t)(vh & 0x03) << 8) | vl),
             ((v6e >> 6) & 1) ? "" : " (thr_src=pin: 写进去不生效!)");
    usb_line("EDX106: reg 0x45=0x%02x delay_src=%s", v45, (v45 & 1) ? "reg" : "pin");
    usb_line("EDX106: reg 0x46..0x49=%02x,%02x,%02x,%02x delay=%ums", dly[0], dly[1],
             dly[2], dly[3], (unsigned)get_le32(dly));
    usb_line("EDX106: reg 0x52..0x53=%02x,%02x lockout=%ums", lk[0], lk[1],
             (unsigned)((uint32_t)lk[0] | ((uint32_t)lk[1] << 8)));
    usb_line("EDX106: reg 0x60=0x%02x gain1(3dB/step, bigger=less gain)", g1);
}

/** SCAN —— 扫 0x08~0x77，本模组应出现在 0x28。 */
static void cmd_scan(void)
{
    if (s_bus == NULL) {
        esp_err_t err = bus_open((uint32_t)CONFIG_APP_EDK60_EDX106_I2C_FREQ_HZ);
        if (err != ESP_OK) {
            usb_line("EDX106: scan err bus=%s scl=%d sda=%d", esp_err_to_name(err),
                     (int)PIN_SCL, (int)PIN_SDA);
            return;
        }
        usb_line("EDX106: scan note=临时创建总线 freq=%u", (unsigned)s_i2c_freq_hz);
    }

    usb_line("EDX106: scan start 0x08..0x77 freq=%u", (unsigned)s_i2c_freq_hz);
    int found = 0;
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, I2C_XFER_TIMEOUT_MS) == ESP_OK) {
            found++;
            usb_line("EDX106: scan found=0x%02x%s", (unsigned)addr,
                     (addr == EDX106_I2C_ADDR_7BIT) ? " (EDX106)" : "");
        }
    }
    usb_line("EDX106: scan done found=%d", found);
    if (found == 0) {
        /* 扫不到时的三步排查（顺序按"最可能/最容易"排），只提示、不阻塞 */
        usb_line("EDX106: scan hint ① 内部上拉是否启用（本固件已在总线配置里开，"
                 "但换过固件/改过代码后要复查）");
        usb_line("EDX106: scan hint ② 降到 50 kHz 再扫一次（内部上拉 ~45k 时上升沿很慢）："
                 "把 APP_EDK60_EDX106_I2C_FREQ_HZ 设成 50000 重新构建");
        usb_line("EDX106: scan hint ③ 仍扫不到 → 建议加 2x4.7k 外置上拉，"
                 "并检查 SCL/SDA 是否接反、3V3_RADAR 是否真的上电");
    }
}

/** TRIG? —— 当前电平、最近触发时刻、累计次数、轮询纠偏次数。 */
static void cmd_trig(void)
{
    int level = gpio_get_level(PIN_OUT);
    usb_line("EDX106: trig? level=%d active=%d last_trig_ms=%" PRId64 " count=%u "
             "last_width_ms=%u poll_resync=%u selfcheck_dropped=%u",
             level, out_level_is_active(level) ? 1 : 0,
             (s_last_rise_us > 0) ? (s_last_rise_us / 1000) : (int64_t)-1,
             (unsigned)s_trig_total, (unsigned)s_last_width_ms, (unsigned)s_poll_resync,
             (unsigned)s_selfcheck_dropped);
}

/** RESET —— 重跑启动序列（软复位 + 重配 + 重新开自检窗口）。 */
static void cmd_reset(void)
{
    usb_line("EDX106: reset cmd -> rerun bringup");
    /* OUT 的引脚配置在入口里已经做过（且模组一直在供电），这里只需重挂中断。
     * 仍先调一次 config_input 是为了"从诊断模式恢复"这种极端路径也不会漏配。 */
    esp_err_t err = out_gpio_config_input();
    if (err != ESP_OK) {
        usb_line("EDX106: err OUT_INIT gpio=%d err=%s", (int)PIN_OUT,
                 esp_err_to_name(err));
    }
    err = bringup_sequence(false);
    if (err != ESP_OK) {
        usb_line("EDX106: mode diag（bringup 未完成：err=%s）", esp_err_to_name(err));
        return;
    }
    err = out_gpio_attach_isr();
    if (err != ESP_OK) {
        usb_line("EDX106: err OUT_INIT gpio=%d err=%s", (int)PIN_OUT,
                 esp_err_to_name(err));
    }
    s_next_stat_us = now_us() + (int64_t)CONFIG_APP_EDK60_EDX106_STATS_SEC * 1000000;
}

/** 严格解析一个十进制 / 0x 十六进制整数：整串必须都是数字，不接受尾随垃圾。 */
static bool parse_num_strict(const char *s, unsigned long *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || end == NULL || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/**
 * `SET mode=normal|ulp` —— **只翻 0x6c 的 bit7，其余位一个都不动**。
 *   normal → 写 0x53（= bits1:0=0b11 + bit6=1 模式走寄存器 + bit4=1 关光敏，bit7=0）
 *   ulp    → 写 0xD3（原默认值，bit7=1）
 * 为什么它是"探测距离不足"的**第一杠杆**：ULP 是 burst / duty-cycle 扫描（手册功耗
 * 78~341 µA），平均发射时间只占很小比例 ⇒ 探测距离必然大幅缩水；常规模式 ≈26 mA，
 * 距离最远。代价就是功耗差两个数量级，所以做成运行期命令，方便现场取舍。
 */
static bool cmd_set_mode(const char *val)
{
    if (!i2c_ready()) {
        usb_line("EDX106: err NOT_READY code=NOT_CONFIGURED hint=\"先 RESET\"");
        return false;
    }

    uint8_t want;
    if (strcmp(val, "normal") == 0) {
        want = REG_6C_NORMAL_VALUE;
    } else if (strcmp(val, "ulp") == 0) {
        want = REG_6C_ULP_VALUE;
    } else {
        usb_line("EDX106: set err key=mode value=%s expect=ulp|normal", val);
        return false;
    }

    esp_err_t err = i2c_wr_reg(REG_CNF_1, want);
    if (err != ESP_OK) {
        usb_line("EDX106: set err key=mode i2c=%s", esp_err_to_name(err));
        return false;
    }
    uint8_t rd = 0;
    if (i2c_rd_reg(REG_CNF_1, &rd) != ESP_OK) {
        usb_line("EDX106: set mode=%s err=READBACK", val);
        return false;
    }
    s_mode_ulp = ((rd >> 7) & 1) != 0;
    usb_line("EDX106: set mode=%s ok rd=0x%02x%s", val, rd,
             (rd == want) ? "" : " (MISMATCH 位型没写进去!)");
    return true;
}

/**
 * `SET gain=<0..15>` —— 写 0x60 的 **bit7:4**，低 4 位保持原值（不擅自清掉别人的位）。
 * 手册：第一级接收增益 3 dB/step，**值越大增益越小**（0=最大增益，15=最小）。
 * 默认 0x33 ⇒ 3。距离不够时可先往小调（4 → 3 → 2），噪声/误触发变多时再往大调。
 */
static bool cmd_set_gain(unsigned long v)
{
    if (!i2c_ready()) {
        usb_line("EDX106: err NOT_READY code=NOT_CONFIGURED hint=\"先 RESET\"");
        return false;
    }
    if (v > 15UL) {
        usb_line("EDX106: set err key=gain value=%lu range=0..15", v);
        return false;
    }

    uint8_t cur = 0;
    if (i2c_rd_reg(REG_GAIN1, &cur) != ESP_OK) {
        usb_line("EDX106: set gain=%lu err=READBACK", v);
        return false;
    }
    uint8_t want = (uint8_t)(((uint8_t)v << 4) | (cur & 0x0F));
    esp_err_t err = i2c_wr_reg(REG_GAIN1, want);
    if (err != ESP_OK) {
        usb_line("EDX106: set err key=gain i2c=%s", esp_err_to_name(err));
        return false;
    }
    uint8_t rd = 0;
    if (i2c_rd_reg(REG_GAIN1, &rd) != ESP_OK) {
        usb_line("EDX106: set gain=%lu err=READBACK", v);
        return false;
    }
    usb_line("EDX106: set gain=%lu ok rd=0x%02x (3dB/step, n 越小增益越大)", v, rd);
    return true;
}

/**
 * `SET win=<len>,<total>,<cnt>` —— 写判决窗口三兄弟：0x0d / 0x30 / 0x31。
 *   len   = 窗口长度（**与存在性探针 0x0d 同一个寄存器**，改过之后重启会看到
 *           `id reg0x0d=… (mismatch, continue)` —— 那是正常的，不是器件坏）
 *   total = 观察窗口数
 *   cnt   = 需要超过阈值的窗口数（默认 3；降到 2 明显更易触发，代价是抗噪变差）
 * 只写、不猜默认值：出厂值本固件不覆盖，`run ... win=` 打的是**回读值**。
 */
static bool cmd_set_win(const char *val)
{
    if (!i2c_ready()) {
        usb_line("EDX106: err NOT_READY code=NOT_CONFIGURED hint=\"先 RESET\"");
        return false;
    }

    char buf[24];
    size_t len = strlen(val);
    if (len == 0 || len >= sizeof(buf)) {
        usb_line("EDX106: set err key=win value=%s expect=<len>,<total>,<cnt>", val);
        return false;
    }
    memcpy(buf, val, len + 1);

    char *parts[3] = { buf, NULL, NULL };
    int n = 1;
    for (char *p = buf; *p != '\0' && n < 3; p++) {
        if (*p == ',') {
            *p = '\0';
            parts[n++] = p + 1;
        }
    }
    if (n != 3) {
        usb_line("EDX106: set err key=win value=%s expect=<len>,<total>,<cnt>", val);
        return false;
    }

    unsigned long v[3];
    for (int i = 0; i < 3; i++) {
        if (!parse_num_strict(parts[i], &v[i]) || v[i] > 255UL) {
            usb_line("EDX106: set err key=win value=%s (三项都要 0..255 的数字)", val);
            return false;
        }
    }

    const uint8_t regs[3] = { REG_WIN_LEN, REG_WIN_TOTAL, REG_WIN_CNT };
    for (int i = 0; i < 3; i++) {
        esp_err_t err = i2c_wr_reg(regs[i], (uint8_t)v[i]);
        if (err != ESP_OK) {
            usb_line("EDX106: set err key=win i2c=%s reg=0x%02x", esp_err_to_name(err),
                     regs[i]);
            return false;
        }
    }

    uint8_t rd[3] = { 0 };
    for (int i = 0; i < 3; i++) {
        if (i2c_rd_reg(regs[i], &rd[i]) != ESP_OK) {
            usb_line("EDX106: set win=%s err=READBACK", val);
            return false;
        }
    }
    usb_line("EDX106: set win=%lu,%lu,%lu ok rd=%u,%u,%u (0x0d/0x30/0x31)", v[0], v[1], v[2],
             (unsigned)rd[0], (unsigned)rd[1], (unsigned)rd[2]);
    return true;
}

/**
 * SET delay=<ms> / thr=<0..1023> / lockout=<ms>：写 + 回读确认。
 * 返回 true = 这一项确实写成功且回读通过（cmd_set 据此决定是否再打一行 run 快照）。
 */
static bool cmd_set_kv(const char *key, unsigned long v, const char *raw)
{
    (void)raw;

    if (!i2c_ready()) {
        usb_line("EDX106: err NOT_READY code=NOT_CONFIGURED hint=\"先 RESET\"");
        return false;
    }

    if (strcmp(key, "delay") == 0) {
        if (v > 600000UL) {
            usb_line("EDX106: set err key=delay value=%lu range=0..600000", v);
            return false;
        }
        uint8_t b[4];
        put_le32(b, (uint32_t)v);
        esp_err_t err = i2c_wr_block(REG_DELAY0, b, sizeof(b));
        if (err != ESP_OK) {
            usb_line("EDX106: set err key=delay i2c=%s", esp_err_to_name(err));
            return false;
        }
        uint8_t rd[4] = { 0 };
        if (i2c_rd_block(REG_DELAY0, rd, sizeof(rd)) != ESP_OK) {
            usb_line("EDX106: set delay=%lu err=READBACK", v);
            return false;
        }
        uint32_t got = get_le32(rd);
        s_delay_ms = got;
        usb_line("EDX106: set delay=%lu ok rd=%u", v, (unsigned)got);
        return true;
    }

    if (strcmp(key, "thr") == 0) {
        if (v > 1023UL) {
            usb_line("EDX106: set err key=thr value=%lu range=0..1023", v);
            return false;
        }
        esp_err_t err = i2c_wr_reg(REG_THR_L, (uint8_t)(v & 0xFF));
        if (err == ESP_OK) {
            err = i2c_wr_reg(REG_THR_H, (uint8_t)((v >> 8) & 0x03));
        }
        if (err != ESP_OK) {
            usb_line("EDX106: set err key=thr i2c=%s", esp_err_to_name(err));
            return false;
        }
        uint8_t lo = 0, hi = 0;
        if (i2c_rd_reg(REG_THR_L, &lo) != ESP_OK ||
            i2c_rd_reg(REG_THR_H, &hi) != ESP_OK) {
            usb_line("EDX106: set thr=%lu err=READBACK", v);
            return false;
        }
        uint32_t got = ((uint32_t)(hi & 0x03) << 8) | lo;
        s_thr = got;
        usb_line("EDX106: set thr=%lu ok rd=%u", v, (unsigned)got);

        /* 顺手确认"阈值走寄存器"这个开关还开着 —— 关着的话上面的写入毫无意义 */
        uint8_t v6e = 0;
        if (i2c_rd_reg(REG_CNF_BURST_THR, &v6e) == ESP_OK) {
            usb_line("EDX106: set thr src=0x6e_bit6=%d%s", (v6e >> 6) & 1,
                     ((v6e >> 6) & 1) ? "" : " (WARN thr_src=pin, 阈值不生效!)");
        }
        return true;
    }

    if (strcmp(key, "lockout") == 0) {
        if (v > 65535UL) {
            usb_line("EDX106: set err key=lockout value=%lu range=0..65535", v);
            return false;
        }
        esp_err_t err = i2c_wr_reg(REG_LOCKOUT_L, (uint8_t)(v & 0xFF));
        if (err == ESP_OK) {
            err = i2c_wr_reg(REG_LOCKOUT_H, (uint8_t)((v >> 8) & 0xFF));
        }
        if (err != ESP_OK) {
            usb_line("EDX106: set err key=lockout i2c=%s", esp_err_to_name(err));
            return false;
        }
        uint8_t lo = 0, hi = 0;
        if (i2c_rd_reg(REG_LOCKOUT_L, &lo) != ESP_OK ||
            i2c_rd_reg(REG_LOCKOUT_H, &hi) != ESP_OK) {
            usb_line("EDX106: set lockout=%lu err=READBACK", v);
            return false;
        }
        uint32_t got = (uint32_t)lo | ((uint32_t)hi << 8);
        s_lockout_ms = got;
        usb_line("EDX106: set lockout=%lu ok rd=%u", v, (unsigned)got);
        return true;
    }

    usb_line("EDX106: set err key=%s unknown", key);
    return false;
}

static void cmd_set(char *args)
{
    bool any = false;      /**< 是否至少识别到一个 key=value（否则打帮助） */
    bool any_ok = false;   /**< 是否至少有一项写成功且回读通过（据此打一行 run 快照） */
    char *p = args;

    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *tok = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p == ' ') {
            *p = '\0';
            p++;
        }

        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            usb_line("EDX106: set err token=%s expect=key=value", tok);
            any = true;
            continue;
        }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;

        /* 字符串型参数先分流（它们的值不是纯数字）：mode=ulp|normal / win=len,total,cnt */
        if (strcmp(key, "mode") == 0) {
            any = true;
            if (cmd_set_mode(val)) {
                any_ok = true;
            }
            continue;
        }
        if (strcmp(key, "win") == 0) {
            any = true;
            if (cmd_set_win(val)) {
                any_ok = true;
            }
            continue;
        }

        char *end = NULL;
        unsigned long v = strtoul(val, &end, 0);
        if (end == val || end == NULL || *end != '\0') {
            usb_line("EDX106: set err key=%s value=%s (不是十进制/0x 数字)", key, val);
            any = true;
            continue;
        }
        if (strcmp(key, "gain") == 0) {
            any = true;
            if (cmd_set_gain(v)) {
                any_ok = true;
            }
            continue;
        }
        any = true;
        if (cmd_set_kv(key, v, val)) {
            any_ok = true;
        }
    }

    if (!any) {
        cmd_help();
        return;
    }
    /* 改完参数立刻回读一行"芯片里实际生效的值"，现场不用再敲 CFG? 去核对 */
    if (any_ok) {
        report_run_state();
    }
}

static void handle_command(char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;   /* 空行：不回应，避免回车刷屏 */
    }

    if (strcmp(line, "CFG?") == 0) {
        cmd_dump_cfg();
    } else if (strcmp(line, "SCAN") == 0) {
        cmd_scan();
    } else if (strcmp(line, "TRIG?") == 0) {
        cmd_trig();
    } else if (strcmp(line, "LED?") == 0) {
        cmd_led_status();
    } else if (strcmp(line, "LED on") == 0) {
        cmd_led_manual(true);
    } else if (strcmp(line, "LED off") == 0) {
        cmd_led_manual(false);
    } else if (strcmp(line, "LED blink") == 0) {
        cmd_led_blink();
    } else if (strcmp(line, "RESET") == 0) {
        cmd_reset();
    } else if (strncmp(line, "SET", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        cmd_set(line + 3);
    } else {
        cmd_help();
    }
}

static void task_cmd(void *arg)
{
    (void)arg;
    char line[CMD_LINE_BUF];
    size_t used = 0;

    while (1) {
        uint8_t ch = 0;
        int got = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(50));
        if (got <= 0) {
            continue;
        }
        if (ch == '\r') {
            continue;                       /* 兼容 \r\n 与单独 \n */
        }
        if (ch == '\n') {
            line[used] = '\0';
            handle_command(line);
            used = 0;
            continue;
        }
        if (used + 1 < sizeof(line)) {
            line[used++] = (char)ch;
        } else {
            used = 0;                       /* 超长行：丢弃并重新同步 */
            usb_line("EDX106: help line_too_long");
        }
    }
}

/* ===========================================================================
 * 13. Demo 入口
 * =========================================================================== */

void demo_door_lock_edx106_start(void)
{
    /* 说明：本 demo **通篇不用 ESP_LOG/printf**（原因见文件头"设计约束"）。
     * 所有对外文本都走 usb_line()，因此日志级别配成什么都不会污染协议流；但为了
     * 彻底干净，仍配套 sdkconfig.defaults.door_lock_edx106 把日志在编译期关掉。 */

    s_usb_lock = xSemaphoreCreateMutex();
    s_out_q = xQueueCreate(16, sizeof(uint32_t));
    if (s_usb_lock == NULL || s_out_q == NULL) {
        return;   /* 连队列/锁都建不出来，无法上报，直接放弃（总比乱跑好） */
    }

    /* --- USB Serial/JTAG：本 demo 的**唯一**通道（上报 + 命令） --- */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_cfg.rx_buffer_size = 2048;
        usb_cfg.tx_buffer_size = 4096;
        esp_err_t uerr = usb_serial_jtag_driver_install(&usb_cfg);
        if (uerr != ESP_OK && uerr != ESP_ERR_INVALID_STATE) {
            return;   /* 没有 USB 就没有这个 demo 的输出通道 */
        }
    }

    /* --- 运行参数快照：一行看完全部编译期配置（上板第一件事就看它） --- */
    usb_line("EDX106: start scl=%d sda=%d out=%d led=%d pwr_en=%d active_high=%d "
             "i2c_freq=%d pullup=internal "
             "thr=%d delay=%d lockout=%d selfcheck=%d stats=%ds mode=%s light=%s led_en=%d "
             "led_follow=%d",
             (int)PIN_SCL, (int)PIN_SDA, (int)PIN_OUT, (int)PIN_LED, (int)PIN_PWR_EN,
             CFG_OUT_ACTIVE_HIGH, CONFIG_APP_EDK60_EDX106_I2C_FREQ_HZ,
             CONFIG_APP_EDK60_EDX106_THRESHOLD,
             CONFIG_APP_EDK60_EDX106_DELAY_MS, CONFIG_APP_EDK60_EDX106_LOCKOUT_MS,
             CONFIG_APP_EDK60_EDX106_SELFCHECK_MS, CONFIG_APP_EDK60_EDX106_STATS_SEC,
             CFG_ULP ? "ulp" : "normal", CFG_LIGHT_OFF ? "off" : "on", CFG_LED_ENABLE,
             CFG_LED_FOLLOW_OUT);

    /* 当前生效值（SET 命令会改它们，上报行用它们） */
    s_thr = (uint32_t)CONFIG_APP_EDK60_EDX106_THRESHOLD;
    s_delay_ms = (uint32_t)CONFIG_APP_EDK60_EDX106_DELAY_MS;
    s_lockout_ms = (uint32_t)CONFIG_APP_EDK60_EDX106_LOCKOUT_MS;
    s_stat_wmin = UINT32_MAX;
    s_mode_ulp = (CFG_ULP != 0);   /* 初值来自 Kconfig；bring-up 回读后以芯片实际值为准 */

    /* --- 橙灯初始化：**无论成功失败都打一行结果**，让"灯到底配没配上"可判定 --- */
    esp_err_t led_err = led_gpio_init();
    usb_line("EDX106: led init gpio=%d active_level=%d enable=%d level=%d follow_out=%d",
             (int)PIN_LED, EDK60_RADAR_LED_ACTIVE_LEVEL, CFG_LED_ENABLE, led_read_level(),
             CFG_LED_FOLLOW_OUT);
    if (led_err != ESP_OK) {
        usb_line("EDX106: err LED_INIT gpio=%d（继续，只是没有橙灯指示）", (int)PIN_LED);
    }

    /* --- ★ 顺序关键：先把 OUT 配成"输入 + 上拉"，**再**给模组上电 ---
     * 模组没上电时 OUT 是高阻；先把上拉打开，引脚从第一刻起就有确定电平
     * （= 高 = 无触发），不会浮空乱跳、也不会被 strapping 采到不确定电平。 */
    esp_err_t err = out_gpio_config_input();
    if (err != ESP_OK) {
        usb_line("EDX106: err OUT_INIT gpio=%d err=%s", (int)PIN_OUT,
                 esp_err_to_name(err));
    }

    /* --- 启动序列（1~7：上电 → I2C → probe → 0x0d → 软复位 → 写配置 → 回读） --- */
    err = bringup_sequence(true);
    if (err != ESP_OK) {
        /* 停在诊断模式：**不重启**，让用户能继续 SCAN / 改线 / 再 RESET。
         * 这是用户明确要求的行为（排查接线比自动重启有用得多）。 */
        usb_line("EDX106: mode diag（启动序列未完成，仅接受 SCAN / CFG? / TRIG? / RESET）");
    } else {
        /* --- 第 8 步：挂双边沿中断（此刻配置已生效、回读已校验） --- */
        err = out_gpio_attach_isr();
        if (err != ESP_OK) {
            usb_line("EDX106: err OUT_INIT gpio=%d err=%s", (int)PIN_OUT,
                     esp_err_to_name(err));
        }
        s_next_stat_us = now_us() + (int64_t)CONFIG_APP_EDK60_EDX106_STATS_SEC * 1000000;
    }

    /* --- 两个任务：监控（永远起）+ 命令（永远起，诊断模式下 SCAN/RESET 才有用） --- */
    if (xTaskCreate(task_monitor, "edx106_mon", 4096, NULL, 6, NULL) != pdPASS) {
        usb_line("EDX106: err TASK monitor create failed");
        return;
    }
    if (xTaskCreate(task_cmd, "edx106_cmd", 4096, NULL, 4, NULL) != pdPASS) {
        usb_line("EDX106: err TASK cmd create failed");
        return;
    }
    usb_line("EDX106: ready（命令：CFG? SCAN TRIG? SET ... RESET）");
}

/* ===========================================================================
 * 14. 编译期保护
 * ---------------------------------------------------------------------------
 * ⚠ 本工程 20 个 demo 的源文件**一起编译**，任何无条件的 #error / _Static_assert
 *   都会让每一次构建（包括其余 19 个 demo）失败（历史坑 B1）。
 *   所以下面**全部**包在 `#if defined(CONFIG_APP_EDK60_DEMO_DOOR_LOCK_EDX106)` 里。
 * =========================================================================== */

#if defined(CONFIG_APP_EDK60_DEMO_DOOR_LOCK_EDX106)

/* ---- ① 引脚合法性 ---- */
_Static_assert(CONFIG_APP_EDK60_EDX106_SCL_GPIO != CONFIG_APP_EDK60_EDX106_SDA_GPIO,
               "SCL 与 SDA 不能是同一根脚");
_Static_assert(CONFIG_APP_EDK60_EDX106_OUT_GPIO != CONFIG_APP_EDK60_EDX106_SCL_GPIO &&
               CONFIG_APP_EDK60_EDX106_OUT_GPIO != CONFIG_APP_EDK60_EDX106_SDA_GPIO,
               "OUT 与 I2C 两根线不能共用引脚");
_Static_assert(CONFIG_APP_EDK60_EDX106_OUT_GPIO != (int)EDK60_PIN_USB_DM &&
               CONFIG_APP_EDK60_EDX106_OUT_GPIO != (int)EDK60_PIN_USB_DP,
               "OUT 不能占用 USB D-/D+（IO12/IO13）");
_Static_assert(CONFIG_APP_EDK60_EDX106_SCL_GPIO != (int)EDK60_PIN_USB_DM &&
               CONFIG_APP_EDK60_EDX106_SCL_GPIO != (int)EDK60_PIN_USB_DP &&
               CONFIG_APP_EDK60_EDX106_SDA_GPIO != (int)EDK60_PIN_USB_DM &&
               CONFIG_APP_EDK60_EDX106_SDA_GPIO != (int)EDK60_PIN_USB_DP,
               "I2C 不能占用 USB D-/D+（IO12/IO13）");
_Static_assert(CONFIG_APP_EDK60_EDX106_SCL_GPIO != (int)PIN_LED &&
               CONFIG_APP_EDK60_EDX106_SDA_GPIO != (int)PIN_LED &&
               CONFIG_APP_EDK60_EDX106_OUT_GPIO != (int)PIN_LED,
               "橙灯 IO15 已被本 demo 用作指示灯，不能与 I2C/OUT 复用");
_Static_assert(CONFIG_APP_EDK60_EDX106_SCL_GPIO != (int)PIN_PWR_EN &&
               CONFIG_APP_EDK60_EDX106_SDA_GPIO != (int)PIN_PWR_EN &&
               CONFIG_APP_EDK60_EDX106_OUT_GPIO != (int)PIN_PWR_EN,
               "雷达电源使能 IO14 必须常高，不能与 I2C/OUT 复用");
_Static_assert(CONFIG_APP_EDK60_EDX106_SCL_GPIO != (int)EDK60_PIN_D0_RX &&
               CONFIG_APP_EDK60_EDX106_SCL_GPIO != (int)EDK60_PIN_D1_TX &&
               CONFIG_APP_EDK60_EDX106_SDA_GPIO != (int)EDK60_PIN_D0_RX &&
               CONFIG_APP_EDK60_EDX106_SDA_GPIO != (int)EDK60_PIN_D1_TX &&
               CONFIG_APP_EDK60_EDX106_OUT_GPIO != (int)EDK60_PIN_D0_RX &&
               CONFIG_APP_EDK60_EDX106_OUT_GPIO != (int)EDK60_PIN_D1_TX,
               "IO16/IO17 是 UART0 控制台（H2 的 D0/D1），不能占用");

/* ---- ② 参数范围（Kconfig 的 range 之外再兜一层，防止手改 sdkconfig） ---- */
_Static_assert(CONFIG_APP_EDK60_EDX106_THRESHOLD >= 0 &&
               CONFIG_APP_EDK60_EDX106_THRESHOLD <= 1023,
               "感应阈值只有 10 bit（0x0f 低 8 位 + 0x10 高 2 位）⇒ 0..1023");
_Static_assert(CONFIG_APP_EDK60_EDX106_LOCKOUT_MS >= 0 &&
               CONFIG_APP_EDK60_EDX106_LOCKOUT_MS <= 65535,
               "闭锁时间只写到 0x52/0x53 两个字节 ⇒ 0..65535 ms");
_Static_assert(CONFIG_APP_EDK60_EDX106_SELFCHECK_MS >= 1000,
               "自检窗口小于 1000 ms 会把手册的 3000 ms 自检期截断，上电就会误报 trig=1");
_Static_assert(CONFIG_APP_EDK60_EDX106_I2C_FREQ_HZ >= 10000 &&
               CONFIG_APP_EDK60_EDX106_I2C_FREQ_HZ <= 400000,
               "AT10LP1T1RDB 的 I2C 上限是 400 kHz；再高必然 NACK");

/* ---- ③ 三处"强制走寄存器"的位型必须逐字节等于厂商参考值 ----
 * 这一条是本 demo 的核心保护：0x6e bit6 / 0x45 bit0 / 0x6c bit6 任何一个被
 * Kconfig 组合搞成 0，阈值/延时/模式就会悄悄退回（模组内部绑定的）pin，
 * 而回读值看起来"写成功"了 —— 是最难查的一类故障，所以在编译期炸掉。 */
_Static_assert((REG_6E_VALUE & REG_6E_BIT_THR_REG) != 0,
               "0x6e bit6 必须为 1：否则阈值由外部 pin 决定，写 0x0f/0x10 完全不生效");
_Static_assert((REG_DELAY_SEL_VALUE & 0x01) != 0,
               "0x45 bit0 必须为 1：否则延时由外部 delay pin 决定，0x46~0x49 不生效");
_Static_assert((REG_6C_VALUE & REG_6C_BIT_MODE_REG) != 0,
               "0x6c bit6 必须为 1：否则工作模式由外部 burst_en pin 决定");

#if defined(CONFIG_APP_EDK60_EDX106_ULP_MODE) && defined(CONFIG_APP_EDK60_EDX106_LIGHT_OFF)
_Static_assert(REG_6C_VALUE == 0xD3,
               "0x6c 默认值必须逐字节等于厂商参考值 0xD3（ULP=1 + 模式走寄存器 + 关光敏 + bits1:0=0b11）");
#endif

/* ---- ③b 运行期 `SET mode=normal|ulp` 的两个目标位型（2026-10-18 新增） ----
 * 这两个值只差 bit7，其余位必须与 0xD3 完全相同 —— 否则切模式会顺手改掉
 * "模式走寄存器 / 关光敏 / bits1:0" 这些已经验证过的位，属于最不该发生的连带伤害。 */
#if defined(CONFIG_APP_EDK60_EDX106_LIGHT_OFF)
_Static_assert(REG_6C_NORMAL_VALUE == 0x53,
               "SET mode=normal 写的 0x6c 必须等于 0x53（bit7=0，其余位与 0xD3 一致）");
_Static_assert(REG_6C_ULP_VALUE == 0xD3, "SET mode=ulp 写的 0x6c 必须等于 0xD3");
#endif
_Static_assert((REG_6C_NORMAL_VALUE & REG_6C_BIT_ULP) == 0,
               "常规模式的 0x6c bit7 必须为 0：否则芯片仍在 ULP burst 扫描，探测距离上不去");
_Static_assert((REG_6C_ULP_VALUE & REG_6C_BIT_ULP) != 0, "ULP 模式的 0x6c bit7 必须为 1");
_Static_assert((REG_6C_NORMAL_VALUE & REG_6C_BIT_MODE_REG) != 0 &&
               (REG_6C_ULP_VALUE & REG_6C_BIT_MODE_REG) != 0,
               "两种模式都必须保持 0x6c bit6=1（工作模式由寄存器控制）");
_Static_assert(REG_6D_VALUE == 0xF8,
               "0x6d 默认值必须逐字节等于厂商参考值 0xF8（adj=1 + 最低功耗档 000）");
_Static_assert(REG_6E_VALUE == 0xE8,
               "0x6e 默认值必须逐字节等于厂商参考值 0xE8（thr_reg=1 + burst_adj=1 + bits4:3=01）");

/* ---- ④ 通道与静默策略的一致性 ---- */
#if defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE)
#error "door_lock_edx106 把原生 USB 当作**唯一**上报/命令通道，不能关闭 USB 第二控制台。关闭 CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG 的后果不只是没输出：实测会让 ESP32-C6 启动卡死、USB 不枚举、必须断电重插（docs/EDK60-pitfalls.md D1）。请改回 CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y。"
#endif

#if CFG_SILENT_LOGS && !defined(CONFIG_LOG_DEFAULT_LEVEL_NONE)
#error "本 demo 的输出是**被解析的文本协议**，日志混进去会制造坏行。已开 APP_EDK60_EDX106_SILENT_LOGS，但 CONFIG_LOG_DEFAULT_LEVEL_NONE 没生效。两种修法任选：① 用 sdkconfig.defaults.door_lock_edx106（build_all.ps1 会自动带上）；② 把 APP_EDK60_EDX106_SILENT_LOGS 设为 n 并自担日志污染。"
#endif

#if CFG_SILENT_LOGS && defined(CONFIG_APP_EDK60_STARTUP_BANNER)
#error "启动横幅是 printf（不受 CONFIG_LOG_DEFAULT_LEVEL 管），会把板名等字符打进 USB 数据流。请关掉 APP_EDK60_STARTUP_BANNER（sdkconfig.defaults.door_lock_edx106 已关）。"
#endif

#endif /* CONFIG_APP_EDK60_DEMO_DOOR_LOCK_EDX106 */
