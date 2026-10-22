# 17/17 全量构建回归归档（G6 证据）

> 2026-10-22 ｜ 归档：项目经理 ｜ 对应 Gate-2 条件 **P1-07（G6 判定证据无归档）**

## 一、结论

| 项 | 值 |
|---|---|
| demo 总数 | **17**（原 16 + 新增 `radar_bridge_edv11p`） |
| 通过 | **17 / 17（100%）** |
| 失败 | 0 |
| 执行方式 | `.\build_all.ps1 -Size`（两轮：13 个一轮 + 4 个补跑） |
| 工具链 | ESP-IDF v6.1.0，target `esp32c6` |

> 备注：第一轮 `-Clean` 在 `deep_sleep` 处因**文件被占用**中断（`Remove-Item` IOException，非编译错误）；清理这 4 个目录后补跑，全部通过。失败原因均为**旧构建缓存指向工程搬迁前的 OneDrive 路径**（`cannot read spec file .../OneDrive/.../picolibc.specs`），与代码无关。

## 二、逐 demo 产物（`build-<demo>/edk60_demos.bin`）

| # | demo | 体积 (KB) | 构建时间 |
|---|---|---|---|
| 1 | adc_read | 187.5 | 2026-10-22 11:23 |
| 2 | ble_scan | 624.5 | 11:26 |
| 3 | ble_uart | 623.5 | 11:25 |
| 4 | blink | 173.4 | 11:21 |
| 5 | button | 177.2 | 11:22 |
| 6 | deep_sleep | 199.9 | 11:31 |
| 7 | hello_board | 174.5 | 11:21 |
| 8 | i2c_scan | 202.6 | 11:27 |
| 9 | led_sequence | 195.8 | 11:27 |
| 10 | nvs_rw | 228.7 | 11:33 |
| 11 | radar_bridge（EDV151 体征） | 211.3 | 11:39 |
| 12 | **radar_bridge_edv11p（本次新增）** | **174.5** | **11:28** |
| 13 | radar_power | 205.7 | 11:26 |
| 14 | uart_echo | 201.5 | 11:23 |
| 15 | usb_serial_bridge | 210.1 | 11:36 |
| 16 | wifi_softap | 907.7 | 11:24 |
| 17 | wifi_sta | 913.3 | 11:24 |

原始 CSV：`logs/edv11p/build-regression-17.csv`（本表由该 CSV 生成）。

## 三、本次新增 demo 的构建与产物

| 项 | 值 |
|---|---|
| app 二进制 | `edk60_demos.bin` = `0x2b9e0` = **178,656 B**（174.5 KB） |
| 分区余量 | 最小应用分区 1 MB，剩余 83% |
| 合并固件 | `merged-binary.bin` = **244,192 B**，烧录偏移 **0x0** |
| SHA256 | `C847423F1FFDF7482934355432907AE1A788533AF121DE997C96658549A7B40E` |
| 一致性 | 工程 `build-ev11p-final/`、`release/edv11p/edk60_edv11p_track.bin`、文档站 `static/edk60/edv11p/edk60_edv11p_track.bin` **三处逐字节一致**（评审官独立核对） |

## 四、回归面（为什么说「不破坏其它 demo」）

| 回归面 | 方法 | 结果 |
|---|---|---|
| 构建期 | 17 个 build 目录全部产出 bin | ✅ |
| 分派期 | `demo_dispatch.c` 仅新增 `#elif defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P)` 分支 | ✅ |
| 配置期 | 新 demo 专用 `sdkconfig.defaults.radar_bridge_edv11p`，工程级 defaults 未改语义 | ✅ |
| 断言期 | 新 `#error` 全部绑定 `#if defined(CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P)` | ✅ |
| 源码期 | 其余 16 个 demo 源文件时间戳仍为 10-14 / 10-17（未被触碰） | ✅ |

## 五、基线可追溯性说明（P1-07 遗留）

该工程仓库当前**仅 `LICENSE` 被 git 跟踪**，其余文件均为 untracked，因此无法用 `git diff` 证明「哪些源码变了」。本归档用「文件时间戳 + 构建产物 + 本表」三件套替代，属**部分**追溯；彻底解决需把工程纳入版本控制（建议作为 Gate-3 交接项）。
