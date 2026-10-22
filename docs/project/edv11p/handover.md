# 结项交接包 — EDK60 × EDV11P 侧装人数统计/轨迹 Demo

> 2026-10-22 ｜ 项目经理 ｜ 状态：**已交付、已上线（静默）、含 2 项已知未闭合项**

---

## 一、一句话交付

用 **EDK60_VA（ESP32-C6）** 把 **EDV11P 侧装雷达**的串口数据（921600）纯字节透传到板载**原生 USB**，
浏览器或桌面端直接看到目标轨迹与人数；30 分钟实测 **15,483 帧、坏帧 0、重同步 0**，帧完整率 100%（P4 口径）。
成品已发布：烧录页 + 在线演示 + 教程 + 应用案例。

## 二、线上入口（可直接用）

| 用途 | URL |
|---|---|
| 网页烧录（零基础） | `https://docs.easydetek.com/edk60/edv11p/flash.html` |
| 网页实时轨迹演示 | `https://docs.easydetek.com/edk60/edv11p/track.html` |
| 固件直链（烧 0x0） | `https://docs.easydetek.com/edk60/edv11p/edk60_edv11p_track.bin` |
| 教程（实施指南） | `https://docs.easydetek.com/opensource/开源硬件/EDK60/track-guide` |
| 应用案例 | `https://docs.easydetek.com/blog/edk60-edv11p-person-tracking` |

镜像（备用）：`https://easydetek.github.io/<同上路径>`。

## 三、交付物清单

### 3.1 固件与工程

| 项 | 位置 | 关键值 |
|---|---|---|
| 桥接 demo 源码 | `components/edk60_app/src/demo_get_started_radar_bridge_edv11p.c` | 纯字节透传、零协议注入、零 printf |
| Kconfig 选项 | `main/Kconfig.projbuild`（menu「EDK60 Demo 参数（EDV11P 透传桥 5.2）」） | 含 `_STATS_VIA_USB`（取证用，默认关） |
| demo 专用 defaults | `sdkconfig.defaults.radar_bridge_edv11p` | 921600、静默、缓冲 16 KiB |
| 合并固件 | `build-ev11p-final/merged-binary.bin`、`release/edv11p/edk60_edv11p_track.bin` | 244,192 B / SHA256 `C847423F1FFDF7482934355432907AE1A788533AF121DE997C96658549A7B40E` / 烧 **0x0** |
| 构建脚本修复 | `build_all.ps1` | R13（stderr 过敏）+ BOM/CRLF + `Write-DemoConfig` 去 BOM |

### 3.2 工具

| 工具 | 用途 |
|---|---|
| `tools/edv11p_frame_check.py` | 帧校验：7 条硬判据、`--selftest`、`--check-banner`、`--raw-dump` |
| `tools/capture_gate2.ps1` | 30 分钟基线采集（纯 ASCII，避免 PS 5.1 编码坑） |
| `tools/plot_gate2_capture.py` | 从原始捕获生成轨迹图/时间线 SVG + 统计表 |
| `release/track.html` | 网页版轨迹演示（满视口布局、默认只读、模拟演示） |

### 3.3 项目档案（`docs/project/edv11p/`）

| 文件 | 内容 |
|---|---|
| `charter.md` / `workflow-plan.md` / `risk-register.md` / `dispatch-log.md` | 章程、计划、风险册（R1~R13）、分派与裁决记录 |
| `analysis-bridge-feasibility.md` | 桥接方案 9 章（控制台洁净唯一路线） |
| `hw-wiring-and-firmware.md` | 接线/供电/型号分支核对 |
| `firmware-implementation-notes.md` | 实现说明 + 13 条 errata |
| `frame-check-test-plan.md` | 8 条用例 + 阈值 + 污染反查法 |
| `test-report-onboard.md` | 上板实测报告（含 30 分钟数据、背压计数器、现场处置） |
| `gate-1-review.md` / `gate-1-disposition.md` | 方案冻结评审与处置 |
| `gate-records/gate-2-review.md` / `gate-2-disposition.md` | 固件与实测评审与处置 |
| `build-regression-17.md` | 17/17 全量回归归档 |
| `recipe-brief.md` / `publish-notes.md` / `release-package-notes.md` | 写作事实底座、发布记录与上线核验 |

### 3.4 对外文档

- `docs/recipe-edv11p-person-tracking.md`（工程内源，10 章 + 3 附录）
- `docs/case-edv11p-person-tracking.md`（工程内源）
- 文档站：`opensource_docs/开源硬件/EDK60/track-guide.md` + `opensource_versioned_docs/version-1.0.0/...`（镜像）+ `blog/2026-10-22-edk60-edv11p-person-tracking.md` + `static/edk60/edv11p/*`

## 四、已验证的关键结论（可复现）

| 结论 | 证据 |
|---|---|
| 模组就是人员定位/侧装版 | 回读 `EDV11P_VA,V1.3.3,0x95F0`；挂高 200 cm、x±500/y[0,800]、高 30~200 cm；`0x70` 带坐标 |
| 双向通路有效 | `SET 0x10=0` 停流并收到 `0xA2`；恢复 `SET 0x10=2` 后复流 |
| 桥接不丢字节 | 30 分钟 15,483 帧 / 坏帧 0 / 重同步 0 / 帧完整率 100%（P4）；msgid 分组跳号 0 |
| 背压计数器全 0 | 60 s 取证、12 次采样、radar→PC 14,653 B；uart/fifo/帧错/USB 写失败/丢弃/短写 **均 0** |
| 数据流洁净 | 稳态 0 个非协议字节（仅采集末帧截断 1 B 与窗口外开机日志 2,970 B） |
| 双端可用 | 桌面端 `human_track` 实机出轨迹；网页端 Edge 连 COM8 可用（已修「按钮永灰」缺陷） |
| 工程无回归 | `build_all.ps1 -Size` 17/17 通过，0 失败 |
| 线上固件可信 | 线上 bin SHA256 与本地逐字节一致 |

## 五、已知未闭合项（**交接必读**）

| # | 项 | 影响 | 建议处置 |
|---|---|---|---|
| 1 | **未做斜装几何标定**（轨迹与实际走位偏差未量化） | 对外**不可**声称「轨迹与实际走位一致」，只能说「轨迹可见、坐标语义与侧装口径吻合（z 为推断）」 | 用户已决定**不做标定、也不修改文章**（2026-10-22）；后续如需对外承诺，补一次卷尺标定即可 |
| 2 | **工程源码基线不可追溯**：该仓库仅 `LICENSE` 被 git 跟踪，其余文件 untracked | 无法用 `git diff` 证明哪些源码变更；本次靠时间戳 + 构建产物 + 归档表替代 | 建议把工程纳入版本控制后补一次基线提交 |
| 3 | CI `Sync to Gitee` 自 2026-10-18 起持续失败（`Deploy build to Gitee Pages` 步骤） | 每次上线需本地 `git push gitee main` + 服务器手工重建 | 已登记，本期不修；建议单独立项 |
| 4 | 无人场景（0 人清零）未验证：30 分钟内人数最小 1 | 不能宣称「无人时人数归零」 | 需要时可做 10 分钟空场基线 |
| 5 | 目标 ID 复用（ID 1 占 99.8% 帧） | 轨迹按 ID 连线**不代表同一自然人** | 已在案例与教程中标注 |

## 六、复现步骤（最短路径）

```powershell
# 1) 环境
Set-ExecutionPolicy -Scope Process Bypass -Force
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1     # ESP-IDF v6.1.0
cd <repo>

# 2) 构建与合并
.\build_all.ps1 -Demo radar_bridge_edv11p
idf.py -B build-radar_bridge_edv11p merge-bin        # 产物烧 0x0

# 3) 烧录（模组必须插 H3）
idf.py -B build-radar_bridge_edv11p -p COMx flash

# 4) 验证
py -3 tools\edv11p_frame_check.py --selftest         # 工具自检
py -3 tools\edv11p_frame_check.py --port COMx --duration 1800   # 30 分钟（需 pyserial）
```

> 关键操作提醒：**模组掉电重启后不会自动上报**，需主机下发一次 `SET 0x05=1`（或网页版点一次「下发配置」）才恢复数据流。

## 七、决策记录（本期）

| # | 决策 | 依据 |
|---|---|---|
| DEC-01 | 做网页版实时轨迹（不做桌面上位机） | 用户选择 |
| DEC-02 | 斜装坐标无需软件旋转（60°/30° 是机械参数） | 上位机注释 + 协议无倾角参数 + 回读为侧装口径 |
| DEC-03 | 控制台洁净：保留 USB 第二控制台 + 全静默 + 零 printf | 两次上板实测 + IDF 源码 |
| DEC-04 | 上报频率单位 50 ms（演示下发 2 = 100 ms） | 协议文档第 73/164-169 行 + 上位机代码 |
| DEC-05 | 网页工具默认只读，写入需显式确认 | 用户选择 |
| DEC-06 | 对外静默上线，不主动宣传 | 用户选择 |
| DEC-07 | 不做几何标定、不改对外文章 | 用户决定（2026-10-22） |

## 八、接手建议顺序

1. 读本文件 + `test-report-onboard.md` + `gate-records/gate-2-review.md`（三份即可建立全貌）。
2. 需要改固件 → 先看 `firmware-implementation-notes.md` 的 errata 表（以代码为准）。
3. 需要发布 → 看 `publish-notes.md`（含线上核验与 CI 缺陷说明）。
4. 需要复现实验 → 按第六节四条命令。
