# 交付物索引 — EDK60 × EDV11P 轨迹 Demo

> 2026-10-21 建档 ｜ 编号规则：`<阶段>-<序号>`，状态：草稿 / 冻结 / 废弃

## 一、文档与决策类

| 编号 | 名称 | 责任角色 | 阶段 | 版本 | 状态 | 路径 | 关联 Gate |
|---|---|---|---|---|---|---|---|
| P0-01 | 项目章程 | 项目经理 | P0 | v1.0 | 冻结 | `docs/project/edv11p/charter.md` | Gate-1 |
| P0-02 | 开发计划（WBS/排期/Gate） | 项目经理 | P0 | v1.0 | 冻结 | `docs/project/edv11p/workflow-plan.md` | Gate-1 |
| P0-03 | 风险登记册 | 项目经理 | P0 起全程 | v1.0 | 活跃 | `docs/project/edv11p/risk-register.md` | 各 Gate |
| P0-04 | 分派记录 | 项目经理 | P0 起全程 | v1.0 | 活跃 | `docs/project/edv11p/dispatch-log.md` | — |
| P0-05 | 桥接可行性分析 | 嵌入式 | P0 | — | 草稿 | `docs/project/edv11p/analysis-bridge-feasibility.md` | Gate-1 |
| P0-06 | 接线/供电/固件核对 | 硬件 | P0 | — | 草稿 | `docs/project/edv11p/hw-wiring-and-firmware.md` | Gate-1 |
| P0-07 | 网页工具设计说明 | 全栈 | P0 | — | 草稿 | `docs/project/edv11p/web-track-tool-design.md` | Gate-1 |
| P2-01 | 上板实测报告 | 测试 | P2 | — | 待建 | `docs/project/edv11p/test-report-onboard.md` | Gate-2 |
| P3-01 | Recipe 开发者教程 | 技术文档 | P3 | — | 待建 | `docs/recipe-edv11p-person-tracking.md` | Gate-3 |
| P3-02 | 应用案例 | 技术文档 | P3 | — | 待建 | `docs/case-edv11p-person-tracking.md` | Gate-3 |

## 二、代码与固件类

| 编号 | 名称 | 责任角色 | 阶段 | 版本 | 状态 | 路径 |
|---|---|---|---|---|---|---|
| F1-01 | 桥接 demo 源码 | 嵌入式 | P1 | — | 待建 | `components/edk60_app/src/demo_get_started_radar_bridge_edv11p.c` |
| F1-02 | Kconfig 选项与分派 | 嵌入式 | P1 | — | 待建 | `main/Kconfig.projbuild`、`components/edk60_app/src/demo_dispatch.c` |
| F1-03 | demo 专用 sdkconfig 覆盖 | 嵌入式 | P1 | — | 待建 | `sdkconfig.defaults.radar_bridge_edv11p` |
| F1-04 | 合并固件（烧 0x0） | 嵌入式 | P1 | — | 待建 | `release/edk60_edv11p_track.bin` |
| F1-05 | PC 帧校验脚本 | 测试 | P1 | — | 待建 | `tools/edv11p_frame_check.py` |
| W1-01 | 网页版轨迹工具 | 全栈 | P0/P2 | — | 待建 | `release/track.html` |

## 三、发布物（文档站）

| 编号 | 名称 | 目标 URL | 责任角色 | 状态 |
|---|---|---|---|---|
| R1-01 | 烧录页 | `/edk60/edv11p/flash.html` | 全栈 | 待建 |
| R1-02 | 网页演示页 | `/edk60/edv11p/track.html` | 全栈 | 待建 |
| R1-03 | 固件 manifest | `/edk60/edv11p/manifest.json` | 全栈 | 待建 |
| R1-04 | 固件 bin | `/edk60/edv11p/edk60_edv11p_track.bin` | 嵌入式 | 待建 |
| R1-05 | Recipe 页面 | `/opensource/开源硬件/EDK60/track-recipe` | 技术文档 | 待建 |
| R1-06 | 应用案例 | `/blog/edk60-edv11p-person-tracking` | 技术文档 | 待建 |

## 四、可追溯性检查清单

- [ ] 每个 Gate 记录引用对应交付物编号
- [ ] 固件 bin 的 SHA256 在 manifest / recipe / 发布记录三处一致
- [ ] 文档站 current 与 versioned 镜像内容一致
- [ ] 所有对外截图/录屏不含无关敏感信息
