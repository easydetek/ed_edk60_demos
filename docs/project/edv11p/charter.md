# 项目章程 — EDK60 桥接 EDV11P 斜装人数统计/轨迹 Demo

| 项 | 内容 |
|---|---|
| 项目代号 | EDK60-EDV11P-TRACK |
| 工程根 | `C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos` |
| 文档站 | `C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com` |
| 章程版本 | v1.0（2026-10-21 制定） |
| 制定人 | 项目经理（Project Orchestrator） |
| 授权人 | 用户（出资与拍板方） |
| 关联项目 | EDK60 + EDV151 体征雷达 demo（同类，已上线；本项目的模板） |

---

## 一、项目目标

| # | 目标 | 度量指标 | 目标值 | 判定证据 | 判定人 |
|---|---|---|---|---|---|
| G1 | EDK60 能把 EDV11P 串口数据零改动透传到 USB | 30 分钟连续运行帧完整率 | ≥ 99.9%（无假同步、无重启） | PC 侧帧校验脚本统计报告 | 测试工程师 |
| G2 | 工程师现成上位机可直接用 | `human_track_v1.4_side_mount.py` 连接后 60 秒内出现稳定人数与轨迹 | 1 次通过，轨迹与实际移动一致 | 实测截图 + 录屏 + 会话日志 | 用户 |
| G3 | 网页版可实时演示 | Chrome/Edge 打开网页 → 连接串口 → 看见轨迹与人数 | 无 JS 报错，刷新后 < 60 秒出图 | 实测录屏 | 用户 |
| G4 | Recipe 可被第三方复现 | 从零读者按 recipe 走通「烧录 → 接线 → 上位机 → 轨迹」 | 无外部求助，步骤无缺失 | 教程自检清单 + 一次走查 | 评审官 |
| G5 | 线上烧录与线上演示上线 | 网页烧录成功 + 网页演示可用 | 用户实机烧录通过 | 用户实测 | 用户 |
| G6 | 工程回归零破坏 | 全量 demo 构建 | 17/17（原 16 + 新增 1）0 error 0 warning | `build_all.ps1 -Size` 汇总表 | 评审官 |

## 二、范围

### 包含
1. **新固件 demo `radar_bridge_edv11p`**：UART1（IO4/IO5，921600）↔ 原生 USB Serial/JTAG 的双向原始二进制透传；雷达 EN（IO14）常高；**不注入任何 EDV11P 协议命令**（配置全部由上位机发）。
2. **控制台洁净性方案**：确保 log/printf 不污染二进制流，且不触发「关闭 USB 第二控制台导致启动卡死」的历史坑。
3. **构建产物**：`build_all.ps1 -Demo radar_bridge_edv11p` + `merge-bin` 合并固件（烧 0x0）。
4. **网页版实时轨迹工具** `track.html`：Web Serial + EDV11P 帧解析 + 侧装坐标变换 + Canvas 轨迹/人数 + 无硬件模拟演示模式。
5. **文档**：开发者 recipe 教程、应用案例（blog/案例页）、烧录指南增量、引脚与接线说明、避坑增量。
6. **文档站发布**：`opensource_docs/开源硬件/EDK60/` 新页面 + `opensource_versioned_docs/version-1.0.0/` 镜像 + `static/edk60/` 资源（固件 bin、flash.html、track.html、manifest.json）。

### 不包含（本期明确不做）
1. **不实现/不改桌面端上位机**：`human_track_v1.4_side_mount.py` 只作为验证工具与下载物，一行不改。
2. 不做 EDV11P 模组固件开发（只做选版与烧录使用）。
3. 不做云端/App/多设备组网、不做数据上传。
4. 不修改 EDK60 硬件、不改板、不飞线（若供电实测不足，走「用户决策」而非私自改板）。
5. 不做感应区/屏蔽区的网页可视化绘制（网页版只做 MVP：轨迹 + 人数；高级配置回桌面工具）。
6. 不做英文站（`i18n/en/`）翻译（与本项目其它缺口一致，列入后续）。

### 待定（含决策截止）
| 事项 | 说明 | 决策截止 |
|---|---|---|
| EDV11P 模组具体烧哪个固件版本 | V121_SX「带人员定位」/ V123_SX / 现有出厂版；取决于哪个版本稳定输出 0x70 目标上报 | P1 上板前 |
| 网页版是否要做录制回放 | MVP 不做；演示可用模拟模式 | 发布前 Gate |

## 三、成功标准与 Gate 映射

| # | 标准 | 验收证据 | 判定人 | 关联 Gate |
|---|---|---|---|---|
| S1 | 桥接方案冻结（含控制台洁净首选方案） | `analysis-bridge-feasibility.md` 9 章齐备 | 评审官 | Gate-1 |
| S2 | 固件构建通过且不破坏其它 demo | 17/17 构建汇总表 + merge-bin 产物 | 评审官 | Gate-2 |
| S3 | 上板实测 30 分钟无丢帧、轨迹正确 | 帧校验报告 + 实测录屏 | 测试工程师 + 用户 | Gate-2 |
| S4 | 网页版与桌面端轨迹一致 | 同一时刻双端截图比对 | 评审官 | Gate-3 |
| S5 | Recipe 三步可复现 | 教程自检清单 | 评审官 | Gate-3 |
| S6 | 文档站构建通过并上线 | 本地 build 日志 + 线上 URL 可访问 | 用户 | Gate-3 |

## 四、关键约束与假设

| 类型 | 内容 | 影响 | 失效后果 |
|---|---|---|---|
| 约束 | ESP-IDF v6.1.0 / esp32c6；沿用「全部编译 + 编译期分派」架构 | 新 demo 必须加进 `demo_dispatch.c` 与 Kconfig choice | 漏加则 demo 不可选或编译失败 |
| 约束 | Kconfig 三硬规则（choice 不产生宏 / `default n` 不生成宏 / defaults 不支持行尾注释） | 写法必须合规 | sdkconfig 未生效、符号未定义 |
| 约束 | 任何 `#error` 必须 `#if defined(CONFIG_APP_EDK60_DEMO_XXX)` 绑定本 demo | 否则其余 16 个 demo 全崩 | 构建全量回归失败 |
| 约束 | 文档站 `onBrokenLinks: 'throw'`、静态资源用 `pathname:///`、文档链接带 `.md`、versioned 镜像同步 | 发布步骤有硬规矩 | 构建失败 / 线上 404 |
| 假设 | EDV11P 串口 921600 8N1、帧头 `0x45 0x44`、上位机不做文本握手 | 桥接可做成纯透传 | 若需握手则方案改写 |
| 假设 | EDK60 该路供电可支撑 EDV11P | 直接用板载供电 | 需外部供电（走变更） |
| 假设 | 用户可提供上板环境（插板 → COM 口） | 实测可执行 | 实测延后，Gate-2 降级为 GO-with-conditions |

## 五、团队与决策权

| 角色 | Agent | 本项目中职责 |
|---|---|---|
| 项目经理 | 本编排者 | 计划、分派、Gate 组织、状态与风险、文档索引 |
| 嵌入式工程师 | opcs-sw-engineer | 桥接固件、Kconfig、构建、上板联调 |
| 硬件工程师 | opcs-hw-engineer | 接线/供电/模组固件版本核对、引脚结论 |
| 全栈工程师 | opcs-fullstack-engineer | 网页版 `track.html` |
| 测试工程师 | opcs-test-engineer | 帧完整率测试、上板验收、网页/桌面一致性比对 |
| 技术文档工程师 | opcs-tech-writer | Recipe 教程与案例 |
| 产品评审官 | opcs-reviewer | Gate-1/2/3 独立把关 |
| 供应链/市场 | opcs-scm-engineer / opcs-marketing | 物料清单与发布文案（按需） |

**决策权**：范围增减、硬件改动、对外发布时间 → 用户；技术方案取舍 → 评审官 + 项目经理；实现细节 → 对应工程师。
