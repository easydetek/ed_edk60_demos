# EDK60 工程避坑清单

> 本文件记录 EDK60 多 demo 工程（ESP32-C6 + ESP-IDF v6.1）开发过程中**实际踩到并已修复**的坑。
> 每条都给出「现象 → 根因 → 处置」，可直接当排障手册用。
> 最后更新：2026-10-15（含雷达体征打通全过程）

---

## 目录

- [A. Kconfig 与构建系统](#a-kconfig-与构建系统)
- [B. 编译期断言与多 demo 共存](#b-编译期断言与多-demo-共存)
- [C. ESP32-C6 GPIO](#c-esp32-c6-gpio)
- [D. USB Serial/JTAG](#d-usb-serialjtagg)
- [E. UART 驱动与并发](#e-uart-驱动与并发)
- [F. 工具链与环境](#f-工具链与环境)
- [G. 雷达硬件与协议（实测结论）](#g-雷达硬件与协议实测结论)
- [H. 排障速查表](#h-排障速查表)

---

## A. Kconfig 与构建系统

### A1. `choice` 不会生成整数宏

**现象**：Kconfig 里写了 `choice APP_X_UART_PORT`，两个成员 `_UART1`/`_UART2`；
C 代码里用 `CONFIG_APP_X_UART_PORT` 取端口号 → `error: 'CONFIG_APP_X_UART_PORT' undeclared`。

**根因**：Kconfig 的 `choice` 只提供**成员布尔符号**，本身不产生值。

**处置**：另加一个派生 `int` config 把选项暴露给 C：

```kconfig
choice APP_EDK60_BRIDGE2_UART_PORT
    prompt "桥接用哪个 UART"
    default APP_EDK60_BRIDGE2_UART1
config APP_EDK60_BRIDGE2_UART1
    bool "UART1"
config APP_EDK60_BRIDGE2_UART2
    bool "UART2"
endchoice

config APP_EDK60_BRIDGE2_UART_NUM
    int
    default 1 if APP_EDK60_BRIDGE2_UART1
    default 2 if APP_EDK60_BRIDGE2_UART2
```

### A2. `default n` 的 bool **不生成** `CONFIG_*` 宏

**现象**：`config APP_X_FEATURE` 是 `bool` + `default n`，C 里 `#if CONFIG_APP_X_FEATURE` 编译报
`'CONFIG_APP_X_FEATURE' undeclared`。

**根因**：Kconfig 语义 —— 未选中的 bool **不定义宏**（连 `=0` 都没有）。

**处置**：C 侧一律用 `#ifdef` 兜底：

```c
#ifdef CONFIG_APP_X_FEATURE
#define FEATURE_ON  1
#else
#define FEATURE_ON  0
#endif
```

> 本工程踩了 **3 次**（`AUTODETECT`、`LOCAL_ECHO`、`NVS_*`）。凡「用户可能关掉」的 bool 都要这么写。

### A3. `sdkconfig.defaults` **不支持行尾注释**

**现象**：`CONFIG_BT_ENABLED=y    # 开蓝牙` 这种写法，**整行被静默忽略**；
然后 `host/ble_gap.h: No such file or directory`。

**根因**：kconfgen 只认整行注释，行尾 `#` 会让整行解析失败且**不报错**。

**处置**：注释必须**独占一行**。改完务必回查生成出来的 `sdkconfig` 里那一项到底有没有生效。

### A4. 复用旧 `sdkconfig` 会导致新增符号不生成

**现象**：Kconfig 里新增了 `APP_X_NEWOPT`，改完代码编译却报 `undeclared`，
而 `sdkconfig.X` 里根本没有这一项。

**根因**：`idf.py` 不会因为 Kconfig 变化就强制重新生成已有的 `sdkconfig`。

**处置**：构建脚本在每个 demo 构建前**删掉**它的生成配置（本工程 `build_all.ps1` 已这么做）。
手工验证时同理：改 Kconfig 后先 `rm sdkconfig.<demo>` 再 build。

### A5. 多 defaults 文件覆盖（有用的技巧）

**用途**：同一个工程里，不同 demo 需要**不同的全局配置**（例如「USB 上要不要出日志」）。

**做法**：`SDKCONFIG_DEFAULTS` 支持分号分隔多个文件，**后者覆盖前者**：

```powershell
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.<demo>" build
```

本工程用它给 `radar_bridge` 单独关掉了本地回环、写死了引脚，
而不影响其它 demo 的默认配置。

### A6. 组件 CMakeLists 阶段读不到 `CONFIG_*`

**现象**：组件 `CMakeLists.txt` 里 `if(CONFIG_APP_X_DEMO_A)` **恒为假**；
`idf_build_get_config()` 在该脚本里也是未定义。

**根因**：IDF 6.1 的组件在**早期展开阶段**解析，此时 `CONFIG_*` 还没注入。

**处置**：不要用 CMake 条件决定「编译哪些源文件」。本工程改为
**全部源文件一起编译 + 在 `demo_dispatch.c` 里用 `#if defined(CONFIG_...)` 做编译期分派**，
再靠链接器 `--gc-sections` 把未选中的 demo 裁掉（已用 `nm` 验证符号确实不存在）。

### A7. 顶层 CMakeLists 读不到 `IDF_TARGET`

**处置**：目标芯片校验放到**组件层**做，不要放项目根。

### A8. `main` 组件二次注册冲突

**现象**：`add_library cannot create target "__idf_main" because another target with the same name already exists`。

**根因**：`main` 目录若没有源文件，会先被建成 INTERFACE 库，再注册就冲突。

**处置**：把 demo 实现移到独立组件（本工程为 `edk60_app`），`main` 只留一句分派调用。

---

## B. 编译期断言与多 demo 共存

### B1. 无条件 `#error` 会误伤**所有** demo ⚠

**现象**：为某个 demo 加了一条「配置不对就报错」的 `#error`，结果**其它 15 个 demo 全部编译失败**。

**根因**：本工程 16 个 demo 源文件**一起编译**，
所以任何**无条件**的 `#error` / `#if` 断言对**每一次构建**都生效。

**处置**：断言必须同时约束「当前选中的是哪个 demo」：

```c
#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE) && \
    defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#error "..."
#endif
```

**验证方式**：改完必须双向验证 ——
① 断言针对的 demo 在错误配置下会失败；② 其它 demo 在同样配置下仍能编译通过。

---

## C. ESP32-C6 GPIO

### C1. 纯 `GPIO_MODE_OUTPUT` 时 `gpio_get_level()` **恒返回 0** ⚠

**现象**：`gpio_set_level(pin, 1)` 之后 `gpio_get_level(pin)` 仍读到 0，
于是「电源使能状态」永远显示为「关闭」，但实际引脚是高的。

**根因**：只配成纯输出时**输入通道是关闭的**，回读值无意义。

**处置**：凡需要**回读**的脚一律用 `GPIO_MODE_INPUT_OUTPUT`：

```c
gpio_config_t cfg = {
    .pin_bit_mask = 1ULL << pin,
    .mode = GPIO_MODE_INPUT_OUTPUT,   /* ← 关键：不是 GPIO_MODE_OUTPUT */
    ...
};
```

**排查提示**：如果**所有**脚（含已知能工作的 LED 脚）都读回 0，
那一定是**测试方法**的问题，不是硬件坏了。

---

## D. USB Serial/JTAG

### D1. 关掉「USB 第二控制台」会让系统**启动卡死** ⚠⚠

**现象**：设 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 后，固件启动**停在**
`sleep_gpio: Enable automatic switching of GPIO sleep configuration` 之后，
连 `main_task: Started on CPU0` 都不打印；**USB 口不再枚举**，必须**物理拔插**才能恢复。

**根因**：该选项与 `USJ_ENABLE_USB_SERIAL_JTAG`（IDF 默认 `y`）联动。
两个控制台都从 USB 撤掉后，USB 模块时钟在启动阶段被关，系统初始化挂住。

**对照证据**：同一块板烧 `hello_board`（该选项保持开启）完全正常。

**处置**：**不要**用「关闭 USB 控制台」来实现「数据流干净」。改用「不产生日志字符」：
把日志级别降低、或把 `printf` 改走日志 / 重定向到 `/dev/null`，
让控制台配置保持工程默认。

### D2. USB 是单一外设：日志与数据会混在同一条流里

**事实**：ESP32-C6 的 USB Serial/JTAG 只有**一个**外设（IO12/IO13），
PC 上只呈现**一个**「USB 串行设备」。CDC 数据与 JTAG 调试是它的**分时复用**。

**推论**：
- JTAG 本身**不干扰**数据（不开调试器就不占链路）；
- 但只要日志被送到 USB，就会与业务数据写进**同一个 FIFO** → 上位机收到混合流。

**处置**：要么让日志走 UART0（H2 的 D0/D1），要么把日志级别压到 0。
配套工具应能**自动区分**日志行与二进制帧（见 `tools/radar_monitor.py`）。

### D3. 打开串口会**复位板子**

**现象**：每次用 pyserial 打开端口，板子就重启，导致「每次抓到的都是启动日志」。

**根因**：打开串口时 DTR/RTS 被拉低，在 ESP32 上会触发自动复位电路。

**处置**：打开后立刻 `setDTR(False)` / `setRTS(False)`；
需要看「稳定运行态」时，把采集窗口放到启动之后（例如采集 40 秒以上）。

---

## E. UART 驱动与并发

### E1. `uart_flush_input()` 与 `uart_read_bytes()` 争锁导致**死锁** ⚠

**现象**：在一项任务里持续 `uart_read_bytes()` 收数据的同时，
另一处调用 `uart_flush_input()` → 后者**永久阻塞**，日志停在那里不再输出。

**根因**：两者都要拿 UART 的接收互斥锁，读任务高频重入导致 flush 拿不到锁。

**处置**：**初始化阶段的握手（发送命令 + 读应答）必须在启动数据搬运任务之前完成**；
运行期不要再去 flush。

### E2. 波特率不匹配时，UART 可能丢弃**全部**字节

**现象**：某根脚在 921600 下是 0 字节，容易被判成「这根脚没信号」。

**事实**：帧错误会让字节被整体丢弃，所以「0 字节」**不能**直接等同于「没有信号」。

**处置**：扫脚之后**还要扫波特率**（本工程 `radar_bridge` 已内置 9 档回退扫描）。

### E3. 悬空脚的噪声字节数可能**超过**真实信号

**现象**：自动识别按「收到的字节数最多」判胜，
结果选中了悬空脚（噪声 167 字节）而放弃了真实信号脚（73 字节）。

**处置**：判据不能只看字节数，应结合「字节种类数 / 是否出现合法帧头（0x5A）」；
**一旦人工确认了正确引脚，就直接写死配置、关掉自动识别**，不要赌启发式。

---

## F. 工具链与环境

### F1. `PRIu32` 在本工具链展开为 `"lu"`

**现象**：`-Werror=format=` 报 `format '%lu' expects 'long unsigned int', but argument has type 'unsigned int'`。

**根因**：本 ABI 下 `uint32_t == unsigned long`，而常量写成 `2u` 是 `unsigned int`。

**处置**：常量显式定型 `((uint32_t)2)`，或改用 `%u` + `(unsigned)` 强转。

### F2. OneDrive 同步盘上 ninja 报权限错误

**现象**：`ninja: error: failed recompaction: Permission denied`。

**处置**：删掉该 demo 的构建目录重建；必要时把构建目录移出同步盘。

### F3. PowerShell 控制台中文乱码

**现象**：脚本输出中文变成 `搴旂敤缁勪欢...`。

**处置**：设 `$env:PYTHONIOENCODING="utf-8"`，或在 Python 里用
`io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")` 包一层。

### F4. PowerShell 传多个 `-Demo` 参数要写成数组

**现象**：`-Demo radar_power,usb_serial_bridge` 被当成一个字符串。

**处置**：用 `-Demo radar_power -Demo usb_serial_bridge`，或脚本里声明为数组。

---

## G. 雷达硬件与协议（实测结论）

> 这一节全部来自 2026-10-15 上板实测，**优先级高于原理图推断**。

### G1. 引脚必须实测，图纸上可能标错

| 信号 | 早期读图推断 | **实测真相** |
|---|---|---|
| 雷达电源使能 EN | IO15 | ❌ **IO14** |
| 雷达 UART RX | IO14 | ❌ **IO4**（H3 的 T 脚） |
| 雷达 UART TX | 以为没有 | ❌ **IO5**（H3 的 A 脚） |
| 橙灯（丝印 L） | = 电源使能脚 | ❌ **仅指示灯**，与电源无关 |

**判定手法**（不需要万用表）：
- 让某个脚**周期性拉高/拉低**，肉眼观察丝印 **RADAR 的红灯**是否同步闪 → 定 EN 脚；
- 把候选脚逐个配成 UART RX 收数据 → 哪根收到合法帧（`0x5A` 开头）就是模组 TX；
- 向候选脚逐个发 `get_version`，看哪根能收到 `"ok"/版本号` 应答 → 就是模组 RX。

### G2. 排针 H3 与 H4 信号相同但**顺序相反**

- **H3** = `RES A R T G V P`（A=IO0 / R=IO1 / T=IO4 / V,P=3.3V_RADAR）
- **H4** = `RES P V G T R A`（T 与 EN 同网络）

**结论：模组必须插 H3**。插 H4 时 T 与 EN 撞在一起，电源与数据互相冲突。

### G3. 扫描候选脚时**绝不能动电源使能脚** ⚠

**现象**：自动识别把 6 个候选脚全扫一遍，结果**全部 0 字节**，像「模组不输出」。

**根因**：扫描时为了「避免把候选线钉死」，把 EN 脚释放成了高阻输入 ——
**一释放就把 3.3V_RADAR 关掉了**，模组整段扫描期间都是断电状态。

**处置**：扫描期间**保持 EN 输出高**，并把 EN 脚本身从候选列表里**剔除**
（扫它等于跟电源使能抢同一根线）。

### G4. 雷达射频**默认关闭**，必须 `turn_on`

**现象**：只上电时，模组只会主动发两类握手帧，永远没有数据：
```
radar_ready   ← 上电初始化完成
get_time      ← 请求主控校时（每小时一次）
```

**处置**：主控必须下发 `turn_on`（`turn_off` 可关）。

### G5. **增益过大是「检测到人但心率呼吸恒为 0」的真凶** ⚠⚠

**现象**：能稳定检测到人（距离 43~58cm），但 `心率=0`、`呼吸率=0`，
`体动` 一直在 **20~60** 之间。

**根因**：体征算法要求**静息体动 < 5** 才肯计算。
出厂 `gain=4 / boost=0` 时灵敏度过高，坐着不动的体动都有 20~60。

**处置**：按官方推荐改成 **`set_gain 0.5` + `set_boost 2`**，
体动降到 **1~6** 后，心率 75bpm、呼吸率 21bpm **立即输出**。

> 这是整个雷达调试里**最难定位**的一条：现象看起来像硬件坏了，
> 实际只是两个参数。

### G6. 帧格式与校验

```
+------+--------+--------+---------------+--------+
| 0x5A |  LEN   |   ID   |    PAYLOAD    |  XOR   |
+------+--------+--------+---------------+--------+
  LEN = 整帧字节数（含帧头与校验）
  ID  : 0x00~0x7F 雷达上报；0x80~0xFF 主控请求（应答沿用请求 ID）
  XOR : 前面所有字节依次异或

实测样例：
  5A 0F 00 "radar_ready" 05     ← LEN=15, TYPE=0x00
  5A 0C 01 "get_time"    6B     ← LEN=12, TYPE=0x01
  5A 06 81 "ok"          D9     ← turn_on 的应答
```

### G7. 体征帧字段

```
vital_signs <睡眠状态>,<距离cm>,<体动>,<心率bpm>,<呼吸率bpm>,
            <心率状态>,<呼吸状态>,<心率可信度>,<呼吸可信度>

睡眠状态：0离床 1清醒 2REM 3浅睡 4深睡 5翻身
心率/呼吸率为 0 表示未识别；可信度 0~4，越高越可信
```

---

## H. 排障速查表

| 现象 | 先查这里 |
|---|---|
| `CONFIG_XXX undeclared` | [A2](#a2-default-n-的-bool-不生成-config_-宏) `default n` 的 bool 不生成宏 → 用 `#ifdef` |
| 改了 Kconfig 但符号没出现 | [A4](#a4-复用旧-sdkconfig-会导致新增符号不生成) 删掉该 demo 的 `sdkconfig` 重建 |
| 某个 demo 改了配置，其它 demo 编不过 | [B1](#b1-无条件-error-会误伤所有-demo-) 编译期断言要加 `defined(CONFIG_APP_EDK60_DEMO_xxx)` |
| 引脚明明给了高，回读却是 0 | [C1](#c1-纯-gpio_mode_output-时-gpio_get_level-恒返回-0-) 改用 `GPIO_MODE_INPUT_OUTPUT` |
| 烧完不启动、USB 不枚举、必须拔插 | [D1](#d1-关掉usb-第二控制台会让系统启动卡死-) 不要关 USB 第二控制台 |
| 上位机收到「数据夹日志」的混合流 | [D2](#d2-usb-是单一外设日志与数据会混在同一条流里) 日志与数据分流 |
| 每次抓日志都只看到启动过程 | [D3](#d3-打开串口会复位板子) 打开串口会触发复位 |
| 日志卡在某一行不再输出 | [E1](#e1-uart_flush_input-与-uart_read_bytes-争锁导致死锁-) UART 锁死锁 |
| 某根脚 0 字节，判不出有没有信号 | [E2](#e2-波特率不匹配时-uart-可能丢弃全部字节) 要连波特率一起扫 |
| 自动识别挑错了脚 | [E3](#e3-悬空脚的噪声字节数可能超过真实信号) 别只看字节数，确认后写死 |
| 雷达引脚跟图纸对不上 | [G1](#g1-引脚必须实测图纸上可能标错) 用「拉高看红灯 / 发命令看应答」实测 |
| 扫描后雷达彻底没数据 | [G3](#g3-扫描候选脚时绝不能动电源使能脚-) 扫描别动 EN 脚 |
| 只有 `radar_ready` / `get_time` | [G4](#g4-雷达射频默认关闭必须-turn_on) 没发 `turn_on` |
| 检测到人但心率呼吸恒 0 | [G5](#g5-增益过大是检测到人但心率呼吸恒为-0-的真凶-) 调小 gain、调 boost |
| `ninja: failed recompaction` | [F2](#f2-onedrive-同步盘上-ninja-报权限错误) 清构建目录 |
| 控制台中文乱码 | [F3](#f3-powershell-控制台中文乱码) 设 `PYTHONIOENCODING=utf-8` |

---

## 配套工具

| 工具 | 用途 |
|---|---|
| `tools/radar_watch.py` | 体征验收：实时显示距离/体动/心率/呼吸，出数即判定 OK |
| `tools/radar_monitor.py` | 通用监视：自动分离日志与雷达帧，`vital_signs` 翻译成中文 |
| `tools/radar_cmd.py` | 直接与雷达模组对话：`get_gain`、`set_boost 2`、`get_para` … |
| `build_all.ps1` | 逐 demo 独立构建 / 全量回归 / 体积统计 |
