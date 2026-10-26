# EDX106 门锁雷达 demo 5.5 —— 固件实现说明

> 2026-10-26 ｜ 嵌入式工程师（opcs-sw-engineer，技能 emb-driver / emb-arch / emb-debug）
> 状态：**固件 + 命令行调参已完成，等上板验证**（按用户决策：先固件，验证通过后再补网页/文档）
> 关联文件：`docs/project/edx106/plan.md`（项目计划）、`docs/project/edx106/edx106-module-and-wiring.md`（硬件判读，含 T-1~T-13 待实测清单）
> **变更记录**：2026-10-26 用户无外部电阻 ⇒ 全程用 **C6 内部上拉**，I2C 默认频率 400 kHz → **100 kHz**，降频兜底改为三级（配置值 → 100k → 50k），OUT（IO2）启用内部上拉并**先配置后上电**。详见 §3.1 / §4 / §10 Plan B。
>
> **变更记录（2026-10-18，本轮）**：① 用户新需求「触发后就亮灯」⇒ 新增 4 条**橙灯诊断命令**（`LED on/off/blink`、`LED?`）+ 新增 Kconfig `APP_EDK60_EDX106_LED_FOLLOW_OUT`（跟随 OUT / 只闪 300 ms），并补 `led init`、`led on (trigger)`、`led off (release)` 三行日志；② 现场反馈「探测距离严重不足」⇒ 新增 `SET mode=` / `SET gain=` / `SET win=` 三条在线调参命令，**阈值默认 300 → 70**（中途在线验证过 115 ≈ 30 cm，用户 `SET thr=70` 回读通过后定稿），demo defaults 里把 `_ULP_MODE` 覆盖为 **n（常规模式）**，并新增一行**回读值**快照 `EDX106: run ...`。详见 §15 / §16。

---

## 0. 一句话结论

MCU 现在是**主动方**：通过 I2C（SCL=IO4 / SDA=IO5）给 AT10LP1T1RDB 写 7 组配置并逐字节回读校验，然后把芯片 OUT（IO2）的电平变化翻译成一行一条的 USB 文本事件；橙灯（IO15）跟随 OUT 亮灭；另有 5 条 USB 命令用于现场诊断与调参。**协议语义、时序、状态机全部在固件里**，这与前四个"透传桥"demo 是两类东西。

---

## 1. 交付文件清单

| # | 文件 | 变更 | 说明 |
|---|---|---|---|
| 1 | `components/edk60_app/src/demo_get_started_door_lock_edx106.c` | **新增**（~1180 行，含注释） | demo 5.5 全部实现 |
| 2 | `components/edk60_app/CMakeLists.txt` | 改 1 处 | `EDK60_DEMO_SRCS` 加一行源文件（19→20） |
| 3 | `components/edk60_app/src/demo_dispatch.c` | 改 2 处 | ① 前置声明 + `#elif` 分支；② "洁净门禁" `#if` 增加本 demo |
| 4 | `main/Kconfig.projbuild` | 改 2 处 | ① choice 加 `APP_EDK60_DEMO_DOOR_LOCK_EDX106`（5.5）；② 文末新增 menu「EDK60 Demo 参数（EDX106 门锁 5.5）」**15 个 config**（本轮新增 `APP_EDK60_EDX106_LED_FOLLOW_OUT`；`THRESHOLD` 默认 300→70） |
| 5 | `sdkconfig.defaults.door_lock_edx106` | **新增** | demo 专用默认配置（静默日志 / 关横幅 / 参数快照），`build_all.ps1` 会自动叠加 |
| 6 | `build_all.ps1` | 改 1 行 | `$DemoMap` 加 `'door_lock_edx106' = 'APP_EDK60_DEMO_DOOR_LOCK_EDX106'`（**保持 UTF-8 BOM + CRLF**） |
| 7 | `docs/project/edx106/firmware-implementation-notes.md` | **新增** | 本文件 |

未改动任何既有 demo 的源码与行为；未删除任何文件。

---

## 2. 架构（为什么是两个任务，而不是一个）

```
                     ┌──────────────────────────────────────┐
   ISR (IO2 双边沿)  │ out_edge_isr()                       │
   只做 2 件事 ─────► │  ① gpio_get_level()  ② xQueueSendFromISR │
                     └───────────────┬──────────────────────┘
                                     │  Queue(16 × uint32)
                                     ▼
   ┌─────────────────────────────────────────────────────────────┐
   │ task_monitor  prio 6 / 4 KB                                 │
   │  · 20 ms 节拍 + 队列唤醒                                     │
   │  · **电平永远以 gpio_get_level() 为准**（中断只是"早点知道"）  │
   │  · 自检窗口丢弃事件 → SELFCHECK done                         │
   │  · 跳变 → usb_line() + 橙灯 + 统计                           │
   │  · **完全不碰 I2C**                                          │
   └─────────────────────────────────────────────────────────────┘
   ┌─────────────────────────────────────────────────────────────┐
   │ task_cmd  prio 4 / 4 KB                                     │
   │  · USB 行输入 → CFG? / SCAN / TRIG? / SET / RESET            │
   │  · **独占全部 I2C 访问**（含 RESET 重跑启动序列）             │
   └─────────────────────────────────────────────────────────────┘
```

**并发模型（关键设计决策）**

- 第一次 bring-up 跑在 demo 入口（`app_main` 任务）里，**在 `task_cmd` 创建之前完成**；之后的 bring-up 只由 `task_cmd`（`RESET` 命令）触发。⇒ **任意时刻只有一个执行流访问 I2C**，因此**故意不加 I2C 互斥锁**（少一把锁 = 少一个死锁点）。若将来把 I2C 搬进监控循环，必须补锁。
- USB 写是多任务共享的 → 用 `s_usb_lock` 串行化。否则两行文本会互相插进对方中间，解析器直接坏掉（文本协议的"坏行"没有校验能发现）。
- `s_selfcheck_until_us` 是 `volatile int64_t`（monitor 读、入口/cmd 写）。64 位读非原子，但语义上只要求"大致到点"，最坏偏差一个 20 ms 节拍。

**为什么 ISR 里不能碰 I2C**：I2C 事务要阻塞等 FIFO/中断，在 ISR 上下文里等待会直接死锁；而且 `i2c_master_transmit()` 内部可能取信号量。ISR 只投递队列，是本工程一贯纪律。

---

## 3. 引脚与接线（用户 2026-10-26 拍板，不得推翻）

| 信号 | EDK60 落脚点 | GPIO | 固件行为 |
|---|---|---|---|
| SCL | H3 的 `T`（原模组 TX） | **IO4** | `i2c_new_master_bus()` 走 GPIO matrix 硬件 I2C |
| SDA | H3 的 `A`（原 MCU TX） | **IO5** | 同上 |
| OUT | H3 的 `A2` | **IO2** | `GPIO_MODE_INPUT` + **内部上拉** + 双边沿中断，**任何时刻都不驱动** |
| V / G | H3 的 `V` / `G` | — | 3.3V_RADAR，由 **IO14** 使能（固件拉高并保持） |
| 橙灯 | 板上丝印 `L` | **IO15** | `GPIO_MODE_INPUT_OUTPUT`，高电平点亮，跟随 OUT |

### 3.1 I2C 上拉：样机阶段只用内部上拉（2026-10-26 用户决定）

| 项 | 现状 | 说明 |
|---|---|---|
| 板上 R13/R14（4.7 kΩ 上拉位） | **DNP 未贴片**，且挂在 IO6/IO7 网络 | 与本次用的 IO4/IO5 **无关**，帮不上忙 |
| 外部电阻 | **用户手上没有 4.7 k / 10 k** | 明确要求：只用内部上拉，不要等外置 |
| 实际生效的上拉 | **C6 内部上拉（~45 kΩ 量级）** | 由 `i2c_master_bus_config_t.flags.enable_internal_pullup = true` 提供，**SDA/SCL 唯一来源** |
| 总线频率默认值 | **100 kHz**（原计划 400 kHz → 已下调） | 45 kΩ + 跳线电容 → 上升沿慢，400 kHz 下几乎必然 NACK |
| 探测失败兜底 | **三级降频**：配置值 → 100 kHz → 50 kHz | 每级都打印 probe 结果；成功时打印**实际使用的频率**（`i2c ok freq=… (已降频)`） |
| 全失败时 | 报 `I2C_NO_DEVICE` + `hint 建议加 2x4.7k 外置上拉` | **只提示不阻塞**，停在诊断模式等 `SCAN`/`RESET` |
| 产品化建议 | H3 侧外置 **2×4.7 kΩ**，频率升回 **400 kHz**（芯片上限） | 不阻塞本次验证；降频逻辑不影响将来升级 |

> 为什么降频**不会**掩盖"到底有没有器件"：probe 是标准地址探测，频率只影响时序余量；三级都失败基本可判定为"没接/接反/没电/上拉严重不足"，此时固件给的提示就是下一步该做什么。

**IO2（OUT）的三条纪律（写进代码注释了）**

1. **只配 `GPIO_MODE_INPUT` + 内部上拉**（`GPIO_PULLUP_ONLY` 语义：上拉、不下拉、不输出），**从不 `gpio_set_level()`**。因为从不驱动，所以不会影响 C6 的 strapping 判定。
2. **配置顺序：先配 IO2（输入+上拉）→ 再拉高 IO14 给模组上电**。理由：模组没上电时 OUT 是**高阻**，先给上拉才能让该脚从第一刻起就有确定电平（= 高 = 无触发），既不会浮空乱跳，也不会在 strapping 采样窗口里悬空。
3. IO2 属 GPIO0~7 → 落在 EXT1 深睡唤醒域内。**将来做"有人靠近才唤醒 MCU"不需要改脚**。

**Plan B（若现场出现上电偶发异常/进下载模式）**：把 OUT 改到 H3 的 **`A3` = IO3**——同属 GPIO0~7（EXT1 可用）、**不是 strapping 脚**、板上无功能网络占用。改动只有两步：Kconfig `APP_EDK60_EDX106_OUT_GPIO=3` + 把模组 OUT 线挪到 H3 的 A3。源码里 `out_gpio_config_input()` 的注释与 Kconfig help 都写了这条。

---

## 4. 启动序列（严格顺序，每步失败都有明确上报）

| # | 动作 | 参数 | 失败上报（USB 行） |
|---|---|---|---|
| 0 | **先把 OUT(IO2) 配成输入 + 内部上拉**（不驱动；**在模组上电之前**） | `GPIO_MODE_INPUT` + `PULLUP` | `EDX106: err OUT_INIT gpio=2 err=…` |
| 1 | IO14 拉高 → 等 ≥100 ms | `RADAR_PWR_SETTLE_MS=100` | `EDX106: err PWR_INIT gpio=14 err=…` |
| 2 | 建 I2C 总线 + 挂 0x28（内部上拉） | `CONFIG_..._I2C_FREQ_HZ`（默认 **100 kHz**） | `EDX106: err I2C_BUS_INIT freq=… scl=4 sda=5 err=…` |
| 3 | `i2c_master_probe(0x28)`，**失败逐级降频**（配置值 → 100k → 50k） | 每级 3 次重试 | `EDX106: probe addr=0x28 fail freq=…` → `EDX106: I2C_DOWNSHIFT 100000 -> 50000 (retry)` → 全失败：`EDX106: err I2C_NO_DEVICE tried=2 lowest_freq=50000 …` + `EDX106: hint 连续 2 次 probe 均失败 → 建议加 2x4.7k 外置上拉`，**停在诊断模式（不重启）** |
| 4 | 读 0x0d 作为存在性探针 | 期望 0x0a | 只打印实际值，不一致不中止：`EDX106: id reg0x0d=0xNN expect=0x0a (mismatch, continue)` |
| 5 | 软复位 0x00=0x00 → 10 ms → 0x00=0x01 → 20 ms | `SOFT_RESET_*` | `EDX106: err CFG_WRITE reg=0x00 err=…` |
| 6 | 写配置块（7 组，见 §5，**0x6e 排在阈值之前**） | 见 §5 | `EDX106: err CFG_WRITE block err=…` |
| 7 | 回读校验 13 个寄存器 | 逐字节 | 每条不一致：`EDX106: CFG_MISMATCH reg=0xNN wrote=0xNN read=0xNN`；并另打控制行 `EDX106: ctrl …` |
| 8 | 挂 OUT 双边沿中断（引脚在第 0 步已配好） | `GPIO_INTR_ANYEDGE` | `EDX106: err OUT_INIT gpio=2 err=…` |
| 9 | 自检窗口内丢弃事件 | `_SELFCHECK_MS`（默认 3000，**从模组上电起算**） | 窗口结束：`EDX106: SELFCHECK done level=<n> dropped=<n>` |
| 10 | 进入监控循环 | 20 ms 节拍 | — |

> 第 0 步与第 8 步在实现上被拆成两个函数（`out_gpio_config_input()` / `out_gpio_attach_isr()`），**就是为了强制"先配引脚、后上电、最后挂中断"这个顺序**——写成一个大函数很容易被后人调换顺序。

### 4.1 为什么"复位 → 释放 → 写全配置 → 回读 → 开中断"

AN-003 §3.1 明写**软复位不改变已设置的寄存器内容**（只重载内部逻辑），而任务书早期的理解是"复位后必须重写全部配置"。两者不冲突时最优的顺序恰好是同一个：

- 按"复位会清空"理解：这个顺序当然正确（写配置在复位之后）；
- 按"复位不清空"理解：这个顺序同样正确（重复写一遍幂等，回读校验还能顺带验证"写入通路"是否正常）。

⇒ **两种理解下都安全**，所以不做分支，固定一个顺序并在代码里写明理由。

### 4.2 为什么自检窗口从"上电"起算而不是"配置写完"起算

芯片自检（0x3c/0x3d 默认 3000 ms）是从**它自己上电**开始的，与 MCU 什么时候写完配置无关。若从"配置写完"起算，窗口会被推迟，等于把芯片自检的最后一段暴露成"误触发"。`RESET` 命令会重开窗口，因为软复位同样会重跑芯片内部自检。

---

## 5. 寄存器表与语义（本固件实际写入的值）

| 寄存器 | 写入值 | 语义（据此写注释与回读校验） | 默认值 |
|---|---|---|---|
| 0x00 | `0x00` → 延时 → `0x01` | 软复位 → 释放 | 0x01 |
| **0x6c** | **0xD3** | bit7=1 超低功耗 / **bit6=1 工作模式由寄存器** / bit4=1 关光敏（兼容保留）/ bits1:0=0b11 | 0x03 |
| **0x6d** | **0xF8** | bit3=1 参数可调 / bits2:0=000 最低功耗档 / bits7:4 按厂商参考值保留 | 0xf3 |
| **0x6e** | **0xE8** | **bit6=1 阈值由寄存器** / bit5=1 burst 可调 / bits4:3=01 / bit7 按参考值保留 | 0x98 |
| 0x0f | `thr & 0xFF` | 阈值低 8 位（默认 thr=300 → 0x2C） | 0x06 |
| 0x10 | `(thr>>8) & 0x03` | 阈值高 2 位（300 → 0x01） | 0x00 |
| **0x45** | **0x01** | bit0=1 **延时由寄存器控制** | 0x00 |
| 0x46~0x49 | `delay` 32 位小端 | 高电平延时，**单位 ms**（默认 2000 → D0 07 00 00） | 0x0400 = 1024 ms |
| 0x52 / 0x53 | `lockout` 16 位小端 | 灭灯保护（闭锁）时间，**单位 ms**（默认 2000 → D0 07） | 0x05e8 ≈ 1512 ms |
| 0x60 | **0x33** | 第一级接收增益（3 dB/step，**值越大增益越小**） | 0x33 |

**回读校验覆盖的 13 个字节**：`0x6c / 0x6d / 0x6e / 0x0f / 0x10 / 0x45 / 0x46~0x49 / 0x52 / 0x53 / 0x60`，逐字节比对，不一致逐条打印 `CFG_MISMATCH`。

### 5.1 三个"缺一不可"的寄存器控制位（本 demo 的核心）

| 位 | 不置 1 的后果 | 为什么在 EDX106 上尤其致命 |
|---|---|---|
| **0x6e bit6** | 阈值由**外部 pin 的 8 档**决定，`0x0f/0x10` **完全不生效**（出厂 0x98 的 bit6=0！） | 模组 5 个脚**没有**引出这个 pin，模组内部绑成什么未知（T-12） |
| **0x45 bit0** | 延时由**外部 delay pin** 决定，`0x46~0x49` 不生效 | 同上，delay pin 也未引出 |
| **0x6c bit6** | 工作模式由**外部 burst_en pin** 决定 | 同上，burst_en 也未引出 |

**"静默失效"为什么最难查**：写 `0x0f=300` 之后回读仍然是 300（写入通路正常），但芯片实际用的是 pin 档位 —— 现象是"参数改了没反应"，很容易误判成"芯片坏"或"接线松"。所以本固件做了三重保护：

1. 写顺序上把 `0x6e` 排在 `0x0f/0x10` **之前**；
2. 启动日志单独打一行 `EDX106: ctrl 0x6c=… 0x6d=… 0x6e=… 0x45=… (thr_reg=… delay_reg=… mode_reg=…)`；
3. 源码里 `_Static_assert` 把"这三位的 bit 组合必须为 1、且默认值逐字节等于 0xD3/0xF8/0xE8"钉死在编译期（见 §9 负控验证）。

### 5.2 0x6c 的两处说明

- **bit4（关光敏）**：本型号 `EDX106-N-01` 没有光敏脚（命名规则 `N` = 无光敏）⇒ 该位实际无作用，写 1 属于**兼容保留**（换成 `-Y` 型号才有意义）。
- **bits[1:0]**：任务书语义写作"bit1:0=1"，但厂商参考值 `0xD3` 的实际位型是 **0b11**。本固件**以参考值的实际位型为准**（固定 `0x03`），不替手册猜这两位的作用，并在代码注释里写明差异。

---

## 6. Kconfig（menu「EDK60 Demo 参数（EDX106 门锁 5.5）」）

| 符号 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `APP_EDK60_EDX106_I2C_FREQ_HZ` | int 10000–400000 | **100000** | 内部上拉下 100 kHz 才稳；失败自动降到 50 kHz |
| `APP_EDK60_EDX106_SCL_GPIO` | int 0–23 | **4** | H3 的 `T` |
| `APP_EDK60_EDX106_SDA_GPIO` | int 0–23 | **5** | H3 的 `A` |
| `APP_EDK60_EDX106_OUT_GPIO` | int 0–23 | **2** | H3 的 `A2`（只配输入 + 上拉；Plan B → 3 = A3） |
| `APP_EDK60_EDX106_OUT_ACTIVE_HIGH` | bool | **y** | 依据 AN-003 §3.4「有感应输出高电平」 |
| `APP_EDK60_EDX106_DELAY_MS` | int 0–600000 | **2000** | 写 0x46~0x49 |
| `APP_EDK60_EDX106_LOCKOUT_MS` | int 0–65535 | **2000** | 写 0x52/0x53（只有 16 位） |
| `APP_EDK60_EDX106_THRESHOLD` | int 0–1023 | **70**（原 300；中途试过 115） | 写 0x0f/0x10；70 = 用户 `SET thr=70` 回读通过后定稿 |
| `APP_EDK60_EDX106_ULP_MODE` | bool | **y**（产品默认） | 0x6c bit7；**demo defaults 覆盖为 n**（调距用常规模式，见 §16） |
| `APP_EDK60_EDX106_LIGHT_OFF` | bool | **y** | 0x6c bit4（兼容保留） |
| `APP_EDK60_EDX106_SELFCHECK_MS` | int 1000–20000 | **3000** | 自检窗口（丢弃事件） |
| `APP_EDK60_EDX106_STATS_SEC` | int 0–3600 | **10** | 1 行/10 s；0 = 关 |
| `APP_EDK60_EDX106_LED_ENABLE` | bool | **y** | 橙灯总开关；n = 所有 LED 命令回 `led disabled` |
| `APP_EDK60_EDX106_LED_FOLLOW_OUT` | bool | **y** | **本轮新增**：y = 灯跟随 OUT；n = 触发后只闪 300 ms |
| `APP_EDK60_EDX106_SILENT_LOGS` | bool | **y** | 与 `CONFIG_LOG_DEFAULT_LEVEL_NONE` 交叉校验 |

**遵守的三条硬规则**

1. 本 menu **不使用 choice** ⇒ 不需要派生 int（choice 不产生值符号）。
2. 所有 bool 在 C 侧用 `#ifdef` 兜底成 0/1 宏（`CFG_ULP` 等），**代码里不出现 `#if CONFIG_xxx`**。
3. `sdkconfig.defaults.door_lock_edx106` 里**注释全部独占一行**，没有行尾注释。

`_SILENT_LOGS` 与 `_LED_ENABLE` 是任务书"建议项"之外新增的两个开关，理由：
- 前者的存在让 `#error` 能明确表达"我要干净流"这个意图，而不是靠人记得改日志级别；
- 后者让"只上报不点灯"成为编译期选择（演示/量产可不同）。

### 6.1 编译期断言（全部绑定本 demo）

源码 §14 的断言**全部**包在 `#if defined(CONFIG_APP_EDK60_DEMO_DOOR_LOCK_EDX106)` 里（本工程 20 个 demo 一起编译，无条件 `#error` 会让每次构建全挂 —— pitfalls B1）。断言清单：

| 类别 | 断言 |
|---|---|
| 引脚 | SCL≠SDA；OUT 不与 SCL/SDA 同脚；三者均不占 IO12/IO13（USB）、IO14（PWR_EN）、IO15（LED）、IO16/IO17（UART0） |
| 范围 | 阈值 0..1023；闭锁 ≤65535；自检 ≥1000 ms；I2C 10000..400000 |
| **控制位** | `0x6e bit6 != 0`、`0x45 bit0 != 0`、`0x6c bit6 != 0` |
| **位型复刻** | `REG_6C_VALUE == 0xD3`（当 ULP 与 LIGHT_OFF 都开）、`REG_6D_VALUE == 0xF8`、`REG_6E_VALUE == 0xE8` |
| 通道 | 禁止 `CONFIG_ESP_CONSOLE_SECONDARY_NONE`（USB 是唯一通道，且关掉会让 C6 启动卡死） |
| 洁净 | 开了 `SILENT_LOGS` 就必须 `LOG_DEFAULT_LEVEL_NONE` 且关掉 `STARTUP_BANNER` |

---

## 7. ASCII 时序图

### 7.1 启动（t=0 是 IO14 拉高那一刻）

```
t(ms)   0      100        130~150      160   170        180              3180
        │       │            │           │     │          │                │
IO14 ───┘‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾（常高）
        │◄─ settle 100ms ─►│
I2C     │      probe 0x28 ─┤ rd 0x0d ─┤ soft reset ─┤ 写配置块 ──► 回读 13 字节
        │                  │          │  10+20 ms   │
OUT     │  ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔ 芯片自检强制高 ≈3000 ms  ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔│
        │  ▲ 这一段的所有事件都被丢弃（SELFCHECK 窗口）                          │
USB     │  boot …→ i2c ok …→ id …→ reset …→ cfg ok …→ ctrl …→ out …→ [SELFCHECK done]
        │                                                                    ▲
        │                                            窗口结束，此刻电平成为基线 ┘
```

`RESET` 命令会把上面从"soft reset"那一段重跑，并**重开**一个 3000 ms 窗口（软复位同样会重跑芯片自检）。

### 7.2 一次触发（delay=2000 / lockout=2000）

```
        t0(有人)                      t0+2000                       t0+4000
OUT ─────┐‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾┌──────────────────────────────
         │      高电平 = 芯片的 delay 定时（0x46~0x49）      │
LED  ────┐‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾┌────  （LED 直接映射 OUT 电平）
USB  ────┤ trig=1 t=t0 …                    │ trig=0 t=t0+2000 width≈2000
         │                                   │
         └── 闭锁（lockout, 0x52/0x53）在这段之后才开始计时 ──►│ 闭锁期内的动作不再触发
```

**LED 定时在芯片里，不在 MCU 里**：固件只做"电平 → 灯"的映射，不做任何 `esp_timer` 定时。这样"灯和 OUT 同步"是结构性正确，不依赖软件定时精度。

---

## 8. 上报协议与调试命令

### 8.1 协议行（每行 `\r\n` 结尾，全部以 `EDX106: ` 开头）

| 行 | 何时 | 内容 |
|---|---|---|
| `EDX106: t=<ms> trig=1 level=1 delay=<ms> lockout=<ms> thr=<n> mode=<normal\|ulp>` | OUT 变有效 | 触发事件（带当前生效参数） |
| `EDX106: t=<ms> trig=0 width=<实测毫秒>` | OUT 失效 | 回落事件（**实测宽度**，不是配置值） |
| `EDX106: stat interval=<s> trig=<n> width_min=<ms> width_max=<ms> i2c_err=<n>` | 每 `_STATS_SEC` | 区间统计（trig/min/max 打印后清零；i2c_err 为累计） |
| `EDX106: cfg ok thr=.. delay=.. lockout=.. mode=.. light=off` | 配置回读全通过 | 配置完成 |
| `EDX106: CFG_MISMATCH reg=0xNN wrote=0xNN read=0xNN` | 某寄存器回读不符 | 每条一行 |
| `EDX106: SELFCHECK done level=<n> dropped=<n>` | 自检窗口结束 | 窗口内丢弃计数 |
| `EDX106: ctrl 0x6c=.. 0x6d=.. 0x6e=.. 0x45=..` | 配置后 | **三个强制控制位的实际值** |
| `EDX106: err <CODE> ...` | 各种失败 | CODE ∈ `I2C_NO_DEVICE / I2C_BUS_INIT / PWR_INIT / ID_READ / CFG_WRITE / OUT_INIT / LED_INIT / NOT_READY / TASK` |
| `EDX106: start …` / `boot …` / `i2c ok …` / `id …` / `reset …` / `out …` / `ready …` | 启动过程 | 参数快照与逐步进度 |
| `EDX106: help …` | 未知命令 / 空 SET | 一行帮助 |

> **一处有意偏离任务书**：stat 行原本写作 `width_min/max=<ms>`（两值一格）。本实现拆成 `width_min=` 与 `width_max=` **两个独立键**，否则解析器拿不到边界值。已在此登记，网页/脚本按新格式解析。

### 8.2 命令清单

| 命令 | 作用 | 是否写芯片 |
|---|---|---|
| `CFG?` | 逐条回读 9 组寄存器并翻译语义（含 `thr_src=pin` 告警） | 否 |
| `SCAN` | 扫 0x08~0x77，标出 0x28 | 否 |
| `TRIG?` | 当前电平 / 最近触发时刻 / 累计次数 / `poll_resync` / `selfcheck_dropped` | 否 |
| `SET delay=<ms>` | 写 0x46~0x49 + 回读确认 | 是 |
| `SET thr=<0..1023>` | 写 0x0f/0x10 + 回读确认 + 顺带复查 `0x6e bit6` | 是 |
| `SET lockout=<ms>` | 写 0x52/0x53 + 回读确认 | 是 |
| **`SET mode=ulp\|normal`** | **只翻 0x6c bit7** + 回读（`set mode=normal ok rd=0x53`） | 是 |
| **`SET gain=<0..15>`** | 写 0x60 **bit7:4**（低 4 位保持）+ 回读 | 是 |
| **`SET win=<len>,<total>,<cnt>`** | 写 0x0d / 0x30 / 0x31 + 三项回读 | 是 |
| **`LED on` / `LED off`** | **只动 IO15**，回 `led on\|off gpio=15 level=<实测读回>` | 否 |
| **`LED blink`** | 亮灭 3 次 × 200 ms，回 `led blink done` | 否 |
| **`LED?`** | `level / on / active_level / cfg_enable / trigger_led / follow_out` | 否 |
| `RESET` | 重跑启动序列（软复位 + 重配 + 重开自检窗口） | 是 |
| 其它 | 打印一行帮助 | 否 |

`SET` 支持一行多个 `key=value`（空格分隔）；写入后**必打印回读值**，例如：

```
EDX106: set delay=3000 ok rd=3000
EDX106: set thr=200 ok rd=200
EDX106: set thr src=0x6e_bit6=1
EDX106: set mode=normal ok rd=0x53
EDX106: set gain=2 ok rd=0x23 (3dB/step, n 越小增益越大)
EDX106: set win=48,64,2 ok rd=48,64,2 (0x0d/0x30/0x31)
```

> 任何一条 `SET` 写成功后，紧跟一行 `EDX106: run ...` 的**回读快照**（见 §15.4 / §16），省掉"再敲一次 `CFG?` 核对"。

---

## 9. 验证记录（本机可执行的部分，全部通过）

### 9.1 `riscv32-esp-elf-gcc -fsyntax-only -Wall -Wextra -Werror`（4 种配置）

模板取自 `build-hello_board/compile_commands.json` 里 `demo_get_started_i2c_scan.c` 的真实编译命令（含全部 `-I`、`-D`、`-march`、picolibc specs），只替换输入文件并追加 `-Wall -Wextra -Werror`。

| # | 配置 | 目标文件 | 结果 | 日志 |
|---|---|---|---|---|
| ① | **量产**：本 demo 选中 + ULP + 光敏关 + 日志静默 | `demo_get_started_door_lock_edx106.c` | **PASS** | 0 行（0 error / 0 warning） |
| ② | **常规功耗**：本 demo 选中 + `ULP_MODE=n` | 同上 | **PASS** | 0 行 |
| ③ | **未选中本 demo + 敌对配置**（hello_board 选中，而 `OUT_GPIO=4 == SCL_GPIO=4`、`THRESHOLD=1023`、`LOCKOUT_MS=65535`、`SELFCHECK_MS=1000`、`DELAY_MS=600000`） | 同上 | **PASS** | 0 行 ⇒ **断言不泄漏到其它 19 个 demo** |
| ④ | 上面 ① 的配置编 `demo_dispatch.c` | `demo_dispatch.c` | **PASS** | 0 行 |
| ⑤ | 附加：③ 的配置编 `demo_dispatch.c` | `demo_dispatch.c` | **PASS** | 0 行（旧 demo 的分派路径未受影响） |

**2026-10-18 复跑（本轮改动后，4 种配置全部 PASS，输出 0 字节）**

| # | 配置（`sdkconfig.h` 由 `python -m kconfgen` 从 Kconfig 真实生成，不是手写宏） | 结果 |
|---|---|---|
| ① | 量产默认：本 demo 选中 + `THRESHOLD=70` + `ULP_MODE` 未设（demo defaults）+ `LED_FOLLOW_OUT=y` | **exit 0 / 0 字节** |
| ② | `# CONFIG_APP_EDK60_EDX106_LED_FOLLOW_OUT is not set`（闪一下） | **exit 0 / 0 字节** |
| ③ | `# CONFIG_APP_EDK60_EDX106_LED_ENABLE is not set`（不开灯） | **exit 0 / 0 字节** |
| ④ | 未选中本 demo + 敌对配置（`OUT_GPIO=4 == SCL_GPIO=4`、`SILENT_LOGS=y` 而日志级别不是 NONE、`ESP_CONSOLE_SECONDARY_NONE=y`、`ULP=y`+`LIGHT_OFF=y`） | **exit 0 / 0 字节** ⇒ 断言与 `#error` 不泄漏到其它 19 个 demo |

负控复跑：N1（把 `0x6e bit6` 改成 0）→ `static assertion failed: "0x6e bit6 必须为 1…"`；N2（把守卫改成 `#if 1` 再用 ④ 的配置）→ 引脚断言 + USB 控制台 `#error` 同时炸 ⇒ ④ 能过**是因为守卫**。

### 9.2 负控（证明断言真的有牙齿，且真的被 `#if defined(...)` 挡住）

| 负控 | 做法 | 期望 | 实测 |
|---|---|---|---|
| N1 | 把 `REG_6E_BIT_THR_REG` 改成 `0x00`（模拟"阈值退回 pin"），配置 ① | 编译失败 | **exit 1**：`static assertion failed: "0x6e bit6 必须为 1…"` + `"0x6e 默认值必须…等于 0xE8"` |
| N2 | 把断言守卫 `#if defined(CONFIG_APP_EDK60_DEMO_DOOR_LOCK_EDX106)` 临时改成 `#if 1`，配置 ③ | 编译失败 | **exit 1**：`static assertion failed: "OUT 与 I2C 两根线不能共用引脚"` + 两条 `#error`（静默策略） |

⇒ N1 说明断言在"该生效时"生效；N2 说明配置 ③ 能通过**是因为守卫**，不是因为断言是死的。

### 9.3 `python -m kconfgen`（Kconfig + defaults 叠加）

用 IDF 生成的 `kconfigs.in` / `kconfigs_projbuild.in` 还原完整的 Kconfig 树，`--defaults` 按 `sdkconfig.defaults → sdkconfig.defaults.door_lock_edx106 → 选中本 demo` 叠加：

- 结果：`Status: Finished successfully`（exit 0），无 ERROR/WARNING（只有 IDF 自身已知 NOTE）。
- **demo 成员数 19 → 20**：`config APP_EDK60_DEMO_*` 共 **20** 条，输出里 19 条 `is not set` + `CONFIG_APP_EDK60_DEMO_DOOR_LOCK_EDX106=y`。
- 各参数默认值：

```
CONFIG_APP_EDK60_EDX106_I2C_FREQ_HZ=100000   CONFIG_APP_EDK60_EDX106_DELAY_MS=2000
CONFIG_APP_EDK60_EDX106_SCL_GPIO=4           CONFIG_APP_EDK60_EDX106_LOCKOUT_MS=2000
CONFIG_APP_EDK60_EDX106_SDA_GPIO=5           CONFIG_APP_EDK60_EDX106_THRESHOLD=70       ← 本轮 300→70（中途试过 115）
CONFIG_APP_EDK60_EDX106_OUT_GPIO=2           # CONFIG_APP_EDK60_EDX106_ULP_MODE is not set  ← 本轮 y→n（demo defaults）
CONFIG_APP_EDK60_EDX106_OUT_ACTIVE_HIGH=y    CONFIG_APP_EDK60_EDX106_LIGHT_OFF=y
CONFIG_APP_EDK60_EDX106_SELFCHECK_MS=3000    CONFIG_APP_EDK60_EDX106_STATS_SEC=10
CONFIG_APP_EDK60_EDX106_LED_ENABLE=y         CONFIG_APP_EDK60_EDX106_LED_FOLLOW_OUT=y    ← 本轮新增项
CONFIG_APP_EDK60_EDX106_SILENT_LOGS=y
```

> 注意 `_ULP_MODE`：**Kconfig 的 `default y` 保留不动（产品默认）**，被覆盖的是 demo 级
> `sdkconfig.defaults.door_lock_edx106` 里的一行 `# CONFIG_APP_EDK60_EDX106_ULP_MODE is not set`。
> 于是"Kconfig 默认 = ULP / 本 demo 默认 = 常规模式"两处**故意不一致**，defaults 文件里有独占注释说明理由
> （现场调距要最大距离；定型后再按功耗预算切回）。

- 静默三件套同时确认生效：`CONFIG_LOG_DEFAULT_LEVEL_NONE=y`、`CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y`、`# CONFIG_APP_EDK60_STARTUP_BANNER is not set`、`# CONFIG_APP_EDK60_PRINT_PINMAP is not set`；`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`（**不是** `_NONE`）。

### 9.4 没能验证的部分（沙箱限制）

`idf.py build` 无法执行（沙箱禁命名管道：IDF 的 CMake/ninja 通过管道抓子进程输出会 EPERM）。因此**真实链接、体积、上板行为都未验证** —— 见 §12 构建命令。

---

## 10. Errata / 已知偏差 / 待实测

| # | 项 | 性质 | 处置 |
|---|---|---|---|
| E1 | `0x6c` bits[1:0]：任务书语义"bit1:0=1" vs 参考值 0xD3 的实际位型 0b11 | **文档与数据的冲突** | 以参考值的实际位型为准（固定 `0x03`），注释写明，`_Static_assert` 钉住 0xD3 |
| E2 | `stat` 行 `width_min/max=` 拆成两个独立键 | 有意偏离协议草案 | 已登记，解析器按新键实现 |
| E3 | `0x6d` bits7:4 与 `0x6e` bit7 的语义未在本次核实范围内 | 未知位 | 按厂商参考值保留（0xF0 / 1），注释标注"未给语义，不擅自改写" |
| E4 | `0x6c` bit4 在 `-N` 型号上无作用 | 型号差异 | 写 1 = 兼容保留，注释说明 |
| E5 | 阈值 300 是**推断值**（手册只给 0~1023 与默认 6） | 推断 | help 里明示"推断起点"，并给现场调法（§11） |
| E6 | 上电后多久可以 I2C 访问（tPOR）手册未给；本固件取 100 ms | 待实测 T-3 | 若上板 probe 偶发失败，先调大 `RADAR_PWR_SETTLE_MS` |
| E7 | 自检窗口 3000 ms 来自 0x3c/0x3d 默认值 | 待实测 T-4 | 实测后可调到"实测值 + 500 ms" |
| E8 | I2C 上拉：板上 R13/R14 为 DNP 且挂在 IO6/IO7，与 IO4/IO5 无关 → **样机阶段只有 ~45 kΩ 内部上拉**（用户无外部电阻） | 已知限制（T-13 仍待实物确认 R13/R14） | 频率默认降到 100 kHz + 三级降频兜底 + 失败提示外置上拉；**量产必须补 2×4.7 kΩ 并升回 400 kHz** |
| E12 | **IO2 是 strapping 关注脚**：app 运行前的电平由芯片内部默认决定，固件管不到 | 架构性风险 | 固件只配输入+上拉、从不驱动；**Plan B = OUT 改到 H3 的 A3（IO3）**（非 strapping、同属 EXT1 域），已验证 Kconfig 一行可改（§13 C13） |
| E9 | **不写任何电流/续航数字**：模组规格书「<55 µA」vs 芯片手册「≈26 mA」差 3 个数量级，峰值两份都没给 | 待实测 T-5 | 代码与文档均无电流注释；对外禁止承诺续航 |
| E10 | OUT 是否真推挽未实测（由"3.3V±0.1V 输出"推断） | 待实测 T-1 | 固件开**内部上拉**（模组未上电时防浮空），若实测为开漏也能工作（只是上升沿慢） |
| E11 | 模组内部把 delay / burst_en / 阈值 8 档绑成什么未知 | 待实测 T-12 | 固件强制走寄存器（§5.1），所以**不依赖**这个未知量 |

---

## 11. 上板判据（60 秒看什么）

### 11.1 期望的启动日志（成功路径，一行一步）

```
EDX106: start scl=4 sda=5 out=2 led=15 pwr_en=14 active_high=1 i2c_freq=100000 pullup=internal thr=70 delay=2000 lockout=2000 selfcheck=3000 stats=10s mode=normal light=off led_en=1 led_follow=1
EDX106: led init gpio=15 active_level=1 enable=1 level=0 follow_out=1
EDX106: out gpio=2 mode=input pull=up active_high=1 (未驱动，先于模组上电配置)
EDX106: boot pwr_en=1 gpio=14 settle=100ms selfcheck=3000ms
EDX106: i2c ok freq=100000 scl=4 sda=5 addr=0x28 pullup=internal
EDX106: id reg0x0d=0x0a expect=0x0a
EDX106: reset 0x00=0x00 ->10ms-> 0x00=0x01 ->20ms
EDX106: ctrl 0x6c=0x53(mode_reg=1 ulp=0 light_off=1) 0x6d=0xf8(pwr_step=0) 0x6e=0xe8(thr_reg=1 burst_reg=1) 0x45=0x01(delay_reg=1)
EDX106: cfg ok thr=70 delay=2000 lockout=2000 mode=normal light=off
EDX106: run mode=normal thr=70 delay=2000 lockout=2000 gain=0x33(0x60) win=10/0/0
EDX106: out irq=anyedge armed gpio=2
EDX106: SELFCHECK done level=0 dropped=1
EDX106: ready（命令：CFG? SCAN TRIG? LED on|off|blink | LED? | SET ... | RESET）
```

> `mode=normal` 与 `0x6c=0x53` 是本轮新默认（demo defaults 把 ULP 覆盖成 n，见 §16）。
> `win=10/0/0` 里的数字是**回读值**：0x0d 出厂 0x0a（=10），0x30/0x31 在部分样片上读回 0x00，
> 所以判据是"三个数与 `SET win=` 写进去的一致"，而不是"看起来合理"。
> 若 `start ... mode=ulp` / `run ... mode=ulp`，说明仍在 ULP 模式 —— 探测距离必然短（§16 第一步）。

（`dropped` 可能是 0~2：自检期 OUT 由高落到低的那一次会计 1。若日志里出现 `I2C_DOWNSHIFT 100000 -> 50000`，说明内部上拉连 100 kHz 都不够 —— 仍可用，但已到边缘。）

### 11.2 功能判据

| # | 判据 | 方法 |
|---|---|---|
| F1 | OUT 空闲低、有人高 | 万用表量 O 对 G；`TRIG?` 对照 |
| F2 | 触发一次 → `trig=1`，约 2000 ms 后 `trig=0 width≈2000` | 手在 1 m 处挥动，看 USB |
| F3 | 橙灯与 OUT 同步亮灭 | 肉眼 |
| F4 | 闭锁期内再触发**不再出现** `trig=1` | 连续挥手，看间隔 |
| F5 | 回读全通过（无 `CFG_MISMATCH`） | 看启动日志 |
| F6 | 三个控制位都为 1（`thr_reg=1 delay_reg=1 mode_reg=1`） | 看 `ctrl` 行 |
| F7 | 断电重上电 10 次，每次 `cfg ok` | 反复插拔 USB |
| F8 | 参数可改：`SET thr=150` → 回读 `rd=150` 且 `0x6e_bit6=1` | 命令 |
| F9 | I2C 在**内部上拉**下稳定：连续 20 次 `CFG?` 无 NACK、`stat` 行 `i2c_err` 不涨 | 命令 + 观察 |
| **F10** | **灯能亮**：`LED on` → 丝印 `L` 亮、`LED?` 的 `level=1`；`LED off` → 灭、`level=0` | `LED on` / `LED off` / `LED?`（§15.3） |
| **F11** | **触发即亮灯**：挥一次手 → `trig=1` 紧跟 `led on (trigger)`，灯亮 2.0~3.0 s | 目视 + 日志（§15.3 第 3 步） |
| **F12** | **模式真的是 normal**：`run mode=normal`、`ctrl … 0x6c=0x53(ulp=0)` | 启动日志；再 `SET mode=ulp`/`normal` 对比 `rd=` |
| **F13** | **1 m 能触发**：`SET mode=normal` + `thr=70`，1 m 站立 >2 s 必触发 | 见 §16 排查顺序 |

### 11.3 排障映射（现象 → 最可能原因）

| 现象 | 先看什么 | 结论 |
|---|---|---|
| `err I2C_NO_DEVICE`（100k 与 50k 都失败） | 内部上拉是否启用（`start … pullup=internal`）？SCL/SDA 是否接反？`boot pwr_en=1`？ | 内部上拉不够/接线/供电三件事；**连续两次失败固件已提示"加 2×4.7k 外置上拉"** |
| `SCAN` 扫不到 0x28 | ① 内部上拉是否启用；② 把 `I2C_FREQ_HZ` 设成 **50000** 重建后再扫；③ 仍不行 → 2×4.7 kΩ 外置 | `SCAN` 会连打三条 hint，按顺序做 |
| 日志有 `I2C_DOWNSHIFT 100000 -> 50000` | 内部上拉连 100 kHz 都勉强 | 能用但到边缘；布线尽量短、地线共好 |
| `id reg0x0d=0xFF` | 器件 NACK / 未接 | 同第一条 |
| `CFG_MISMATCH reg=0x6e` | 阈值控制位没写进去 | **阈值一定不生效**，先解决这一条再调参 |
| 一上电就 `trig=1` 且久久不落 | 自检窗口太短 | 调大 `_SELFCHECK_MS`（T-4 实测后定值） |
| 触发但 `width` 远小于 `delay` | `0x45 bit0` 没生效（延时还走 pin） | 看 `ctrl … 0x45=0x01(delay_reg=1)` |
| `SET thr=… ok rd=…` 但灵敏度没变化 | `0x6e bit6=0` | `CFG?` 会打 `thr_src=pin: 写进去不生效!` |
| 事件行里夹着中文/板名 | 日志没关干净 | 检查 `sdkconfig.defaults.door_lock_edx106` 是否被叠加 |
| `poll_resync` 一直涨 | 边沿丢失（接线长/抖动） | 固件已自动纠偏；频繁则查接线与地 |
| **上电偶发进下载模式 / 启动异常** | IO2 是 strapping 关注脚 | **Plan B：OUT 改到 H3 的 A3（IO3）**，Kconfig `OUT_GPIO=3` + 挪线（§10 E12） |

---

## 12. 构建命令（请项目经理执行）

```powershell
# 0) 激活 IDF（每次新开终端）
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1

# 1) 切到工程目录
cd C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos

# 2) 单 demo 构建（build_all.ps1 会自动叠加 sdkconfig.defaults.door_lock_edx106，
#    并删掉旧的 sdkconfig.door_lock_edx106 重新生成）
.\build_all.ps1 -Demo door_lock_edx106

# 3) 确认 demo 清单是 20 个
.\build_all.ps1 -List

# 4) 全量回归（20/20 必须全 OK，证明没影响其它 19 个 demo）
.\build_all.ps1 -Size

# 5) 生成「单文件、烧 0x0」的合并固件（可选）
idf.py -B build-door_lock_edx106 merge-bin

# 6) 烧录 + 看日志（COMx 换成实际端口；USB-C 口即原生 USB CDC）
idf.py -B build-door_lock_edx106 -p COM6 flash monitor
#    也可以用任意串口助手打开该 COM 口（波特率随便填），直接敲 CFG? / TRIG? 等命令
```

⚠ 两点提醒：
1. `build_all.ps1` 每次构建都会**删掉该 demo 的 sdkconfig 后重建**（这是设计行为，避免新符号不生效）。**不是**删除源码，不需要额外确认。
2. 本 demo 的 USB 输出是**协议流**，用 `idf.py monitor` 看日志时看到的就是 `EDX106:` 行 —— 这是正常的（日志级别已在编译期关掉）。

---

## 13. 上板前需要用户确认的接线检查项（**请逐条打勾**）

| # | 检查项 | 判据 | 备注 |
|---|---|---|---|
| C1 | 模组插在 **H3**（不是 H4） | H3 丝印 `RES A R T G V P` | H4 的 `T` 与 EN 同网，插错会电源/数据互相干扰（本项目实测教训） |
| C2 | **SCL = H3 的 `T` = IO4** | 万用表通断：模组 SCL ↔ IO4 | 与旧 demo 的 UART RX 同脚，**不能同时跑别的雷达 demo** |
| C3 | **SDA = H3 的 `A` = IO5** | 同上 ↔ IO5 | 同上 |
| C4 | **OUT = H3 的 `A2` = IO2** | 同上 ↔ IO2 | IO2 是 strapping 关注脚；**固件已开内部上拉**（模组未上电时防浮空），不需要外部电阻 |
| C5 | **V / G = H3 的 `V` / `G`**（3V3_RADAR / GND） | 上电后丝印 `RADAR` 红灯亮 | IO14 拉高才有 3V3_RADAR |
| C6 | **不需要外接 I2C 上拉**（本次决定：全程用 C6 内部上拉） | — | 板上 R13/R14 是 DNP 且挂在 IO6/IO7，与本次无关；**代价是只能用 100 kHz（必要时 50 kHz）** |
| C6b | 跳线尽量**短**、地与模组共好 | SCL/SDA 走线 ≤ 10~15 cm | 内部上拉 45 kΩ 时上升沿本来就慢，线越长越容易 NACK |
| C7 | 上电顺序：先 GND → 再 VCC → 最后 SCL/SDA/OUT | 拆线顺序相反 | 防经 IO 灌电流 |
| C8 | 模组天线面朝向：金属门/面板要做塑料窗，与金属保持 **5~12 mm** | 见 `edx106-module-and-wiring.md` §5.2 | 紧贴金属会灵敏度崩 |
| C9 | 模组与 ESP32-C6 天线拉开距离（≥1 m 于路由器，板上也尽量远） | 目视 | 手册第 6 条 |
| C10 | 供电纹波 ≤50~100 mV（怀疑误触发时用示波器量 H3 的 V） | 手册第 3 条 | 纹波超标会误报（T-8） |
| C11 | USB 线是**数据线**（不是纯充电线），插在带数据口的 USB-C 上 | PC 出现「USB 串行设备 (COMx)」 | 本 demo 的唯二通道之一 |
| C12 | 不要同时插 EDV11P/EDV163 等其它雷达模组 | — | 共用 IO4/IO5 |
| C13 | **Plan B 备查**：若上电偶发异常/进下载模式 | 改 `APP_EDK60_EDX106_OUT_GPIO=3` + OUT 线挪到 H3 的 **A3** | IO3 非 strapping、同属 EXT1 域；改完重构建即可，无需改代码 |

---

## 14. 一句话交接

**上板只需要看三行**：`i2c ok`（通信通）、`ctrl … 0x6e=0xe8(thr_reg=1) … 0x45=0x01(delay_reg=1)`（三个控制位真的置上了）、`cfg ok`（回读全通过）。这三行过了，剩下的都是调参数（`SET thr=` / `SET delay=`），不用再改固件。

---

## 15. 橙灯行为与诊断命令（2026-10-18 新增）

### 15.1 三种状态

| 状态 | 触发条件 | 亮多久 | 怎么切 |
|---|---|---|---|
| **① 跟随 OUT**（默认） | `LED_FOLLOW_OUT=y` | 等于 OUT 高电平宽度 = 芯片 `delay`（实测 **2360 ms 量级**） | 保持 Kconfig 默认即可 |
| **② 闪一下** | `LED_FOLLOW_OUT=n` | **300 ms**（上升沿点亮 → monitor 时间戳判定到点熄灭，**不用定时器中断**） | Kconfig 改 n，或 `# CONFIG_..._LED_FOLLOW_OUT is not set` |
| **③ 手动** | `LED on` / `LED off` / `LED blink` | 由命令决定（blink = 亮灭 3×200 ms） | 运行期随时可用，**只动 IO15** |

三条纪律（都在代码里守着）：

1. 手动命令**不碰 I2C、不影响 OUT 中断、不写任何寄存器** —— 灯是纯 GPIO 输出，与雷达通路完全解耦。
2. **手动与触发不互斥**：手动 `LED on` 之后再来一次触发，上升沿仍把灯置"亮"（`led_set()` 是纯电平写，幂等），下降沿仍会熄灭。
3. `LED_ENABLE=n` 时所有 LED 命令**不静默**：一律回一行
   `EDX106: led disabled (CONFIG_APP_EDK60_EDX106_LED_ENABLE=n)`，
   否则"敲了命令没反应"会被误判成固件坏了。
4. `FOLLOW_OUT=n` 时，**自检窗口结束那一刻的电平不算触发**，不点灯（否则会出现
   "灯亮着但日志里没有任何 `trig=1`"的诡异状态）；跟随模式下仍按旧行为同步电平。

### 15.2 命令用法（USB CDC 行输入，`\r\n` 结尾）

```
LED on        →  EDX106: led on gpio=15 level=1        ← level 是**实测回读电平**
LED off       →  EDX106: led off gpio=15 level=0
LED blink     →  亮灭 3×200 ms → EDX106: led blink done
LED?          →  EDX106: led? level=1 on=1 active_level=1 cfg_enable=1 trigger_led=false follow_out=1
```

`LED?` 每个字段的含义：

| 字段 | 含义 | 现场怎么用 |
|---|---|---|
| `level` | `gpio_get_level(IO15)` 实测值（`-1` = 橙灯被 Kconfig 关掉） | **最关键**：随命令变化 ⇒ GPIO 通路正常；不变 ⇒ GPIO 没生效 |
| `on` | 按 `active_level` 换算后"是否点亮" | 与肉眼一致 |
| `active_level` | 点亮电平（本板 = **1**，高电平点亮） | 换板/换灯时先看这个 |
| `cfg_enable` | `LED_ENABLE` 编译开关 | 0 ⇒ 命令只会回 disabled |
| `trigger_led` | 当前这次点亮是否由**触发**引起（手动命令会置 false） | 区分"灯亮是有人"还是"我刚敲的命令" |
| `follow_out` | `LED_FOLLOW_OUT` 编译开关 | 0 ⇒ 触发后只亮 300 ms，别误判成"灯坏了" |

### 15.3 实测验证步骤（3 步，5 分钟内）

1. `LED on` → **看板上丝印 `L` 的橙灯是否亮**；回执里 `level=1` 表示 GPIO 已经输出高电平。
2. `LED off` → 灯灭，回执 `level=0`。
3. 手在传感器前挥一下 → 日志出现
   `EDX106: t=… trig=1 …` **紧跟** `EDX106: led on (trigger)`，灯亮约 **2.0~3.0 s**；
   随后 `trig=0 width=…` + `EDX106: led off (release)`。

**若现场灯不亮，按这个顺序排查**（一次就能定位到层）：

| 现象 | 说明 | 结论 |
|---|---|---|
| `LED?` 的 `level` **随命令变化**（0↔1） | GPIO 通路完全正常 | **问题在灯具侧**：LED 本体/限流电阻/焊接，别再怀疑固件 |
| `level` **不随命令变化** | GPIO 没生效 | ① `cfg_enable=0`？（Kconfig 关了）② IO15 被别的任务/外设复用（本工程别的 demo 用 IO15 做 `RADAR_PWR_EN`）③ 换固件前先确认烧的是本 demo |
| 命令直接回 `led disabled` | `LED_ENABLE=n` 编译进去了 | 打开 `CONFIG_APP_EDK60_EDX106_LED_ENABLE=y` 重新构建 |
| 命令正常、灯亮，但**触发时**不亮 | 灯没问题，是**根本没触发** | 看 `TRIG?` 的 `count`、`SELFCHECK done` 之后才开始计数；再按 §16 解决探测距离 |

### 15.4 三行新日志（既有字段一个没动）

```
EDX106: led init gpio=15 active_level=1 enable=1 level=0 follow_out=1     ← 启动，紧跟 start 行
EDX106: led on (trigger)                                                  ← 紧随 trig=1 行
EDX106: led off (release)                                                 ← 紧随 trig=0 行
```

- `trig=1` / `trig=0` 两行的字段与顺序**完全保持原样**（上游解析器依赖），新增信息一律走独立行。
- `FOLLOW_OUT=n` 时还会多一行 `EDX106: led off (flash 300ms)`（到点熄灭，早于 `trig=0`）。
- 另有一行**回读快照**（bring-up 末尾 + 每条成功 `SET` 之后）：

```
EDX106: run mode=normal thr=70 delay=2000 lockout=2000 gain=0x33(0x60) win=10/0/0
```

  `mode` 来自 0x6c bit7 回读、`thr` 来自 0x0f/0x10、`delay` 来自 0x46~0x49、`lockout` 来自 0x52/0x53、
  `gain` 来自 0x60、`win` 来自 0x0d/0x30/0x31 —— **全是芯片里的实际值**，不是 Kconfig 值。

---

## 16. 探测距离不足的排查顺序（现场反馈：阈值 115 ≈ 30 cm，目标 1 m；最终定稿 70）

> 现场数据（2026-10-18）：阈值 300 → 要贴很近；`SET thr=115`（回读 `rd=115` 通过）→ 约 **30 cm**；`SET thr=70`（回读 `rd=70` 通过）→ 用户定稿为默认值 **70**。
> 结论先说：**阈值不是第一瓶颈，功耗模式才是**。下面按「代价从低到高」排，逐条给预期效果与副作用。

| # | 动作 | 预期效果 | 代价 / 副作用 |
|---|---|---|---|
| **1** | **`SET mode=normal`**（0x6c bit7 1→0，`0xD3`→**`0x53`**） | **最大杠杆**。ULP 是 burst / duty-cycle 扫描（手册 78~341 µA），常规模式 ≈26 mA，探测距离回到芯片标称水平 | **功耗涨到 26 mA 量级**（电池门锁要重新算续航）；发热略增 |
| **2** | `SET thr=`**70 → 60 → 40** | 灵敏度线性提高，1 m 站稳更易触发 | 误触发上升（走过走廊、窗帘、风扇都可能触发）；低于 ~40 后噪声底噪开始主导 |
| **3** | `SET win=<len>,<total>,**2**`（0x31 由 3 → 2） | "需超阈值的窗口数"减一，**明显更易触发**（等效再降一点阈值） | 抗噪变差：单窗偶然尖峰就会被判成"有人" |
| **4** | `SET gain=<0..15>`（0x60 **bit7:4**，3 dB/step，**值越大增益越小**） | 调小（4→3→2）增益变大 → 距离增加 | 增益变大同时放大噪声与本振泄漏，容易误触发；调大则相反 |
| **5** | 检查**安装朝向 / 壳体** | 10.5 GHz 穿透极差：金属完全挡死，塑料壳要留窗、离金属 5~12 mm（§13 C8） | 纯结构改动，无电气副作用 |
| **6** | 检查**电源纹波**（H3 的 V 对 G） | 纹波 >100 mV 会同时造成**误报**与**灵敏度下降**（手册第 3 条，T-8） | 需要示波器；对策是加去耦/换 LDO |

**建议的现场脚本（照抄即可）**：

```
SET mode=normal          → EDX106: set mode=normal ok rd=0x53
                           EDX106: run mode=normal thr=70 ...    ← 先确认模式真的切了
（此时手在 1 m 处试；若已 OK，到此为止）
SET thr=60               → EDX106: set thr=60 ok rd=60
SET win=<len>,<total>,2  → EDX106: set win=...,2 ok rd=...,...  ← 仍不够再降 cnt
SET gain=2               → EDX106: set gain=2 ok rd=0x23 (3dB/step, n 越小增益越大)
```

两条容易踩的坑：

- **`SET mode=ulp` 之后行为会回退**：设置不落 NVS，**复位/重新上电即恢复到编译期默认**（本 demo 默认已是 normal）。现场调好后要固化，得改 `sdkconfig.defaults.door_lock_edx106` 并重新构建烧录。
- **改过 `0x0d`（`SET win=`）之后重启**，启动日志会出现
  `EDX106: id reg0x0d=0x0a expect=0x0a (mismatch, continue)` ——
  这**不是器件坏**：`0x0d` 同时是"窗口长度"和本固件拿来当存在性探针的寄存器。判据依旧看
  `i2c ok` / `ctrl … thr_reg=1` / `cfg ok` 三行。

**测距对照实验的记录方式**：用 `tools/edx106_capture.py` 抓事件并落 CSV（每次触发一行：宽度、间隔），
每换一组参数（模式/阈值/窗口/增益）就加一个 `--label`，例如

```powershell
py -3 tools\edx106_capture.py --port COM8 --duration 60 --label "mode=normal thr=70 gain=0x33 1.0m"
```

这样"距离 vs 参数"的结论是**数据**而不是印象。判据：每行宽度 ≈ `delay`；两次 `trig=1` 的间隔 ≥ `delay + lockout`（否则说明闭锁没生效）。

---

## 17. 探测距离实测结论（2026-10-27 现场定稿）

### 17.1 决定性对比：功耗模式才是瓶颈
同一颗模组、同一套接线（内部上拉 100 kHz）、同一安装位置，仅改工作模式：

| 配置 | 阈值 | 实测触发距离 | 备注 |
|---|---|---|---|
| **ULP 模式**（`0x6c=0xD3`，burst/duty-cycle） | 300 | 需贴很近 | 初始默认（照厂商参考代码） |
| **ULP 模式** | 115 | **≈30 cm** | 降阈值只换来一点距离 |
| **ULP 模式** | 70 | （在线下发 `rd=70` 成功，未单独测距） | — |
| **常规模式**（`0x6c=0x53`，≈26 mA） | **70** | **≥1.0 m 稳定触发** | **本次定稿配置** |

> 结论：**探测距离由占空比决定，不由阈值决定**。ULP 与常规的电流差约 **100 倍**（78~341 µA vs ≈26 mA），1 m 级距离**只能常供电**。
> 若产品必须电池供电，需接受 ~0.3 m 的近距离方案（届时 `SET mode=ulp` 或把 defaults 的 `_ULP_MODE` 改回 y）。

### 17.2 定稿参数（已写入 `sdkconfig.defaults.door_lock_edx106`）
```
CONFIG_APP_EDK60_EDX106_ULP_MODE  未设置        → 常规模式（距离优先）
CONFIG_APP_EDK60_EDX106_THRESHOLD 70            → 实测 1 m 稳定触发
CONFIG_APP_EDK60_EDX106_DELAY_MS  2000          → 实测高电平 2.3~6.6 s（二次触发顺延）
CONFIG_APP_EDK60_EDX106_LOCKOUT_MS 2000         → 实测间隔下限 ≈ delay+lockout，0 次违例
CONFIG_APP_EDK60_EDX106_LED_FOLLOW_OUT y        → 用户确认"每次触发都看到灯亮"
CONFIG_APP_EDK60_EDX106_I2C_FREQ_HZ 100000      → 内部上拉下稳定（无需外部电阻）
```

### 17.3 现场验收记录（用户确认）
- ✅ **1.0 m 处持续挥手 → 22 秒内 3 次触发**（2.37 s / 8.75 s / 16.81 s，宽度 2368 / 3198 / 3845 ms）
- ✅ **橙灯随触发点亮**（用户目视确认）
- ✅ 闭锁防抖：0.5 s 间隔连挥 3 次只记 1 次事件
- ✅ `SCAN` 见 `0x28`、`CFG?` 13 寄存器回读全通过、`SET` 命令逐条回读确认

### 17.4 测量方法学（供复现）
1. 异步指令下无法对齐"用户实际开始时刻"，因此**一次只测一个固定距离**（站定后持续挥手 18 s）。
2. 用 `tools/edx106_capture.py --port COMx --duration 22 --label <距离>_<参数>` 抓取；
   输出含：触发次数、宽度 min/max/avg、相对 delay 偏差、触发间隔与闭锁违例、`rel_s` 相对时间轴。
3. 每次改参数后，`SET` 的回执里那一行 `run mode=… thr=… gain=… win=…` 就是"实际生效值"，抄进 label 即可对照。
4. 注意：开机自检期（≈3 s）OUT 恒高，其下降沿会以 `trig=0 width=1xx` 出现在日志里，**属正常，不是误触发**（固件已在自检窗口内丢事件）。
---

## v1.1.0 变更记录（2026-10-27）

**决策**：按用户要求，**固件默认改为超低功耗（ULP）模式**（`CONFIG_APP_EDK60_EDX106_ULP_MODE=y`，Kconfig 与 demo defaults 两处一致）。

| 版本 | 默认模式 | 实测距离 | 适用 |
|---|---|---|---|
| v1.0.0 | 常规（≈26 mA） | **≥1.0 m** | 常供电门锁 |
| **v1.1.0（当前）** | **ULP（78~341 µA）** | **≈30 cm**（thr=115 实测；thr=70 待实测） | 电池门锁 |

- 固件包：**254,736 B / SHA256 `6BCB87A6…8EDCE96`**
- 距离与功耗是**互斥**关系（芯片占空比机制），运行期 `SET mode=normal` 可切换但掉电失效；固化需改 `sdkconfig.defaults.door_lock_edx106`。
- 文档（recipe / 站点 guide / flash.html）均已加「v1.1.0 默认 ULP」与距离对照说明。
- **未做**：ULP + thr=70 的距离实测（建议现场补测一次，写入本文件）。