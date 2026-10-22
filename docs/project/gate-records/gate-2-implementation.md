# Gate-2 评审记录 — 15 个 Demo 实现

| 项 | 内容 |
|---|---|
| Gate 名称 | Gate-2 实现（G2 / G3 / G4 的验收） |
| 评审日期 | 2026-10-14 |
| 评审对象 | 15 个 demo 源码、多 demo 选择机制、构建脚本 |
| 参与角色 | 项目经理（组织 + 汇总）、评审官（独立把关 / 红队视角）、硬件工程师（引脚与硬件结论复核） |
| 结论 | 见第六节 |

---

## 一、入场条件核对（Gate-1 放行条件闭环）

| # | Gate-1 放行条件 | 状态 | 证据 |
|---|---|---|---|
| C1 | 把「雷达电源极性」更正同步到 BSP / demo / README / pinmap / 风险登记册 | ✅ 已关闭 | CR-06；BSP `edk60_radar_power_*` 改为推挽语义、`ACTIVE_LEVEL=1`、默认关闭 |
| C2 | `radar_power` demo 按更正后语义返工 | ✅ 已关闭 | 子代理终报：旧表述（上拉/默认使能/开漏/0.5mA）已 grep 清零 |
| C3 | 15 个 demo 全部编译通过 | ✅ 已关闭 | 见 [build-verification.md](build-verification.md) |

---

## 二、交付物检查（15 个 demo）

| # | demo | 源文件 | 行数 | 导出函数 | 编译 | 结论 |
|---|---|---|---|---|---|---|
| 1.1 | hello_board | `demo_get_started_hello_board.c` | 81 | `demo_hello_board_start` | ✅ | 通过 |
| 1.2 | blink | `demo_get_started_blink.c` | 67 | `demo_blink_start` | ✅ | 通过 |
| 1.3 | button | `demo_get_started_button.c` | 95 | `demo_button_start` | ✅ | 通过 |
| 1.4 | uart_echo | `demo_get_started_uart_echo.c` | 136 | `demo_uart_echo_start` | ✅ | 通过 |
| 1.5 | adc_read | `demo_get_started_adc_read.c` | 186 | `demo_adc_read_start` | ✅ | 通过 |
| 2.1 | wifi_sta | `demo_get_started_wifi_sta.c` | 931 | `demo_wifi_sta_start` | ✅ | 通过 |
| 2.2 | wifi_softap | `demo_get_started_wifi_softap.c` | 548 | `demo_wifi_softap_start` | ✅ | 通过 |
| 2.3 | ble_uart | `demo_get_started_ble_uart.c` | 896 | `demo_ble_uart_start` | ✅ | 通过 |
| 2.4 | ble_scan | `demo_get_started_ble_scan.c` | 964 | `demo_ble_scan_start` | ✅ | 通过 |
| 3.1 | radar_power | `demo_get_started_radar_power.c` | 474 | `demo_radar_power_start` | ✅ | 通过（含歧义自检） |
| 3.2 | led_sequence | `demo_get_started_led_sequence.c` | 278 | `demo_led_sequence_start` | ✅ | 通过 |
| 3.3 | i2c_scan | `demo_get_started_i2c_scan.c` | 262 | `demo_i2c_scan_start` | ✅ | 通过 |
| 4.1 | deep_sleep | `demo_get_started_deep_sleep.c` | 929 | `demo_deep_sleep_start` | ✅ | 通过 |
| 4.2 | nvs_rw | `demo_get_started_nvs_rw.c` | 1108 | `demo_nvs_rw_start` | ✅ | 通过（危险操作三重保险） |
| 4.3 | usb_serial_bridge | `demo_get_started_usb_serial_bridge.c` | 406 | `demo_usb_serial_bridge_start` | ✅ | 通过 |
| 5.1 | radar_bridge | `demo_get_started_radar_bridge.c` | 约 700 | `demo_radar_bridge_start` | ✅ | 通过（含引脚/波特率自动识别） |

**合计约 9100 行 demo 代码**（不含 BSP 与框架）。

---

## 三、多 demo 选择机制检查

| 检查项 | 方法 | 结果 |
|---|---|---|
| menuconfig 选项齐全 | `main/Kconfig.projbuild` 的 choice 中 demo 项 | ✅ 16/16 |
| 切换后固件行为真的改变 | 对比 `build-blink`（173 KB）与 `build-wifi_sta`（913 KB）的 `.bin` | ✅ 体积与内容均随选择变化 |
| 未选中的 demo 不占固件 | `nm edk60_demos.elf` 查其他 demo 符号 | ✅ 全部查无（`--gc-sections` 生效） |
| Kconfig 结构完整 | 7 个 menu，无重名、`menu`/`endmenu` 配平 | ✅ |
| 目标芯片守卫 | 非 esp32c6 时配置阶段报错 | ✅ 组件层校验 |
| 依赖声明完整 | 各 demo 用到的组件均在 `components/edk60_app/CMakeLists.txt` 的 REQUIRES 中，无需 `idf_component.yml` | ✅ |

---

## 四、红队视角：主动找问题

| # | 红队攻击点 | 检查结果 | 定级 |
|---|---|---|---|
| A1 | 断言「引脚结论一定对」 | **已发现两处真实错误**：①雷达电源极性一度读反（已 CR-06 更正）；②U4.EN 网络标号存在歧义（已 CR-07 登记）。说明「单页原理图能唯一确定接线」这个前提**不成立**，必须靠实测 | P1 |
| A2 | 断言「编译通过 = 能用」 | 全部 15 个 demo **只做了编译/链接级验证，未上板**。已把 11 项上板实测项（H-01~H-11）逐条写成可执行步骤并交接 | P1 |
| A3 | 断言「USB 转串口已实现」 | ⚠ **实际只有「雷达→USB」一个有效方向**：C6 的 UART1 无 IOMUX 默认脚，TX 未布线，因此「USB→雷达」的字节到不了引脚。文档已如实修正，不再宣称双向 | P1 |
| A4 | 断言「蓝牙一定省内存」 | 蓝牙常开（15 个 demo 一起编译的必然结果）⇒ 即使选 `hello_board`，NimBLE/Wi-Fi 的代码也被编进组件（但被链接器裁掉）。代价是编译时间，已在组件 CMakeLists 与风险登记册记录 | P2 |
| A5 | 断言「日志与桥接数据能共存」 | `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` 使日志会混入 USB 桥接数据流；建议做纯桥接时关闭该选项或提高日志级别（已写入交接说明与 demo 注释） | P2 |
| A6 | 断言「深睡一定省电」 | 深睡期间 LED1（绿，直挂 3.3V）与各稳压器静态电流仍在耗；LED4/LED5 已随雷达电源熄灭。**未做实测**，只给了测法 | P2 |
| A7 | 断言「NVS 演示安全」 | 擦除类操作三重保护（Kconfig 默认关 + 超长按 3s + 5s 内二次确认），且只作用于本 demo 命名空间；`nvs_flash_init` 满页时**只提示不擦** | ✅ 通过 |
| A8 | 断言「按键唤醒一定能用」 | C6 的 EXT1 仅 GPIO0..7，IO9(BOOT) 不在其中；代码对该 API 失败**自动降级为定时器兜底**并打印原因，不 assert、不睡死 | ✅ 已容错 |
| A9 | 断言「回调里不做阻塞」 | 各 demo 的 GPIO/NimBLE/Wi-Fi 回调均只置标志/入队，重活放任务；已抽查 wifi_sta / ble_uart / radar_power / deep_sleep | ✅ 通过 |
| A10 | 断言「没有硬编码引脚」 | 对 15 个文件 grep `GPIO_NUM_<数字>`，仅 BSP 与必要的 USB 引脚常量存在 | ✅ 通过 |

---

## 五、本阶段暴露并已修复的构建系统问题（11 类）

这些是「工程骨架能否用」的真正门槛，全部已在代码/文档中留痕：

| # | 问题 | 根因 | 处置 |
|---|---|---|---|
| 1 | Kconfig 顶部用 `>` 当注释 | kconfgen 不支持 | 改 `#` |
| 2 | 顶层 CMake 读不到 `IDF_TARGET` | IDF 6.1 该属性要到组件阶段才有值 | 校验移到组件层 |
| 3 | `cmake_language(DEFER DIRECTORY)` 下宏读不到变量 | DEFER 会切换作用域 | 去掉 `DIRECTORY` |
| 4 | 组件里 `if(CONFIG_xxx)` 恒假 | 组件在「早期展开」阶段解析，此时 `CONFIG_*` 尚未注入；`idf_build_get_config()` 在该脚本里也未定义 | 改为「全部编译 + 编译期分派」 |
| 5 | `main` 组件二次注册冲突 | 无源文件时先建成 INTERFACE 库 | demo 实现移入独立组件 `edk60_app` |
| 6 | `sdkconfig.defaults` 行尾注释导致整行失效 | Kconfig defaults 不支持行尾注释 | 注释独占一行（BT 配置因此一度失效） |
| 7 | BLE 头文件找不到 | 默认配置未开 `CONFIG_BT_ENABLED` / `CONFIG_BT_NIMBLE_ENABLED` | 补 8 项 BT/NimBLE 配置 |
| 8 | `PRIu32` 与实参类型不匹配 | 本工具链 `uint32_t == unsigned long`、`PRIu32` 展开为 `"lu"`，而常量写成 `2u`（`unsigned int`） | 常量定型为 `(uint32_t)`；`%u` + `(unsigned)` 保持不变 |
| 9 | `choice` 不生成整数宏 | Kconfig 语义：choice 只提供其成员的布尔符号 | 另加派生 `int` config（`..._UART_NUM`）把选项暴露给 C 代码 |
| 10 | `default n` 的 bool 不生成 `CONFIG_*` 宏 | Kconfig 语义：未选中即不定义 | C 代码用 `#ifdef` 兜底（同 nvs_rw 的处理方式） |
| 11 | 复用旧 `sdkconfig` 导致新增符号不生成 | `idf.py` 不会因 Kconfig 变化而强制重新生成已有 sdkconfig | `build_all.ps1` 每次构建前删除该 demo 的生成配置 |

---

## 六、Gate 结论

> **GO-with-conditions**
>
> 16 个 demo 全部实现并编译通过（0 error），多 demo 选择机制按设计工作（切换真的改变固件行为、未选中 demo 被链接器裁掉）；Kconfig 与依赖声明完整；红队审查发现的实质问题均为「需要上板验证」或「需要如实体现在文档里」，**没有需要推翻设计的缺陷**。
> 放行条件：

| # | 条件 | 责任角色 | 关闭时点 |
|---|---|---|---|
| C4 | 上板实测 H-01（**使能脚与数据线到底是 IO14 还是 IO15**）—— 可用 demo 5.1 `radar_bridge` 的自动识别一步定论 | 用户 | Gate-4 |
| C5 | 上板实测 H-06（IO12/IO13 能否枚举 USB） | 用户 | Gate-4 |
| C6 | 上板跑通基础外设组 5 个 demo | 用户 | Gate-4 |
| C7 | （已达成）确认 `build_all.ps1` 全量 16/16 通过 | 项目经理 | — |
| C8 | 若 H-01 结论为「数据线是 IO15」，则修订 BSP 宏 + radar_power/usb_serial_bridge/radar_bridge 的引脚默认值与 UART 方案 | 项目经理 | Gate-4 后 |

> **评审官备注**：本次最值得记录的不是「15 个 demo 编过了」，而是三件**差点被当成已完成**的事：
> ①雷达电源极性读反（靠高分辨率重读图 + 交叉复核抓出）；
> ②`U4.EN` 网络标号歧义（单页图纸无法唯一确定，必须实测）；
> ③「USB 转串口」实际是单向的（TX 未布线）。
> 三者都已如实写进文档与风险登记册，而不是留在对话里。
