# 任务分派记录 — EDK60 × EDV11P 轨迹 Demo

## 批次 B1（2026-10-21，三路并行）

| # | 角色 | 任务 | 模式 | 输出物 | 状态 |
|---|---|---|---|---|---|
| T0-1 | 嵌入式工程师 | 桥接可行性与实现方案（9 章，不动代码） | 并行后台 | `docs/project/edv11p/analysis-bridge-feasibility.md` | ✅ 完成（54,889 B） |
| T0-2 | 硬件工程师 | 接线/供电/模组固件版本核对 | 并行后台 | `docs/project/edv11p/hw-wiring-and-firmware.md` | 运行中 |
| T0-3 | 全栈工程师 | 网页版实时轨迹工具 MVP（含模拟模式） | 并行后台 | `release/track.html`（72,766 B）、`web-track-tool-design.md`（30,097 B）、`release/track-selftest.md` | ✅ 完成（自测 5/5 + 18/18） |
| T1-1/2/3 | 嵌入式工程师 | 新 demo 实现 + Kconfig + 构建 + merge-bin | 并行后台 | 源码 27,983 B 已落盘；Kconfig/dispatch/build_all 已登记；defaults 已建 | 运行中 |
| T1-4 | 测试工程师 | 帧校验脚本 | 并行后台 | `tools/edv11p_frame_check.py`（61,784 B） | 运行中（已可自测） |

## 裁决记录（Decision Log）

| # | 日期 | 事项 | 决定 | 证据 | 影响 |
|---|---|---|---|---|---|
| DEC-01 | 10-21 | 线上演示形态 | 做网页版实时轨迹（Web Serial + Canvas），不做桌面上位机 | 用户选择 | 新增 `track.html` 交付物 |
| DEC-02 | 10-21 | 斜装坐标是否需软件旋转 | **不需要**；60°/30° 是机械安装参数，固件已完成 XYZ 转换 | 上位机头部注释 + `INSTALLATION_PROFILES` | 网页端零旋转，只按 tilted 显示口径 |
| DEC-03 | 10-21 | 控制台洁净路线 | 保留 USB 第二控制台 + `LOG_DEFAULT_LEVEL_NONE` + 关横幅/引脚表 + 源码零 printf（诊断直写 UART0 默认关） | `sdkconfig.defaults.radar_bridge` 两次实测 + IDF `esp_stdio/Kconfig:57-61` | 唯一可行路线，已写入固件与方案 |
| DEC-04 | 10-21 | 上报频率单位 | **50 ms/单位**（不是 10 ms）；演示默认下发 **2 = 100 ms** | 协议文档第 73 行 + 第 164-169 行示例 + 上位机 `:1562` | 网页工具默认值改为 2；recipe 写明换算 |
| DEC-05 | 10-21 | 参考文件编码 | 上位机 .py 与协议 md 均为 **UTF-8（无 BOM）**，非 GBK | 本机 strict-UTF8 解码复核 | 后续分派不得再用 GBK 读取 |
| DEC-06 | 10-21 | 模组型号分支 | 用户确认手持模组为 **EDV11P-N-02（人数统计/轨迹跟踪）** | 用户答复 + 发布说明幻灯片 8 | R11 由 15 降至 5，仍需回读固件版本号 |

## 分派要点（自包含信息基线，供后续批次复用）

- 板侧事实：雷达 UART1，MCU RX=IO4 / TX=IO5，EN=IO14（高有效），LED=IO15；**模组必须插 H3**（H4 顺序相反）。
- 协议事实：921600 8N1 无流控；大端；帧头 `0x45 0x44`；长度 2B（整包）+ msgid 2B + op 1B + type 1B + TLV 长度 2B + value + 校验 1B（逐字节求和 & 0xFF）。
- 上位机行为：连上 100ms 后 GET 0x02，再批量 GET 基础/高级配置；无文本握手；工作模式 0x05 的 1 = 跟踪/人数统计。
- 工程硬规矩：全部 demo 一起编译 + 编译期分派；`#error` 必须绑定 `CONFIG_APP_EDK60_DEMO_XXX`；Kconfig 三硬规则；不得关闭 USB 第二控制台。
- 参考文件编码：一律 **UTF-8（无 BOM）**，不得按 GBK 读取。

## 待发批次

| 批次 | 任务 | 前置 | 状态 |
|---|---|---|---|
| B2 | T1-1 固件实现、T1-2 Kconfig/分派、T1-4 帧校验脚本 | T0-1 | ✅ 已发 |
| B3 | T1-3 构建与合并固件 | T1-1/T1-2 | 进行中 |
| B4 | T2-1~T2-4 上板实测（需用户插板） | B3 | 阻塞（无 COM） |
| B5 | T3-1~T3-4 文档与 release 包 | B4 | 待发 |
| B6 | T4-1~T4-4 文档站发布 | B5 | 待发 |
