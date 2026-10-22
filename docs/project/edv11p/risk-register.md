# 风险登记册 — EDK60 × EDV11P 轨迹 Demo

> 2026-10-21 首次登记 ｜ 项目经理维护 ｜ 评分：概率 P / 影响 I，1~5 分，R = P×I

| ID | 风险 | P | I | R | 触发信号 | 缓解措施（owner） | 状态 |
|---|---|---|---|---|---|---|---|
| R1 | **控制台日志污染二进制流**：IDF 默认把 log 打到 USB Serial/JTAG，与透传共用端点，上位机按 `0x45 0x44` 同步可能误判（"EDK60" 首两字节正是 ED） | 4 | 5 | **20** | 上位机偶发丢帧/坏帧；日志与帧交错出现 | **已收敛为唯一路线**：保留 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` 不动（`SECONDARY_NONE` 实测启动卡死）+ `LOG_DEFAULT_LEVEL_NONE=y` + 关 `STARTUP_BANNER`/`PRINT_PINMAP`（printf 型）+ 源码零 printf（诊断直写 UART0、默认关）；用帧校验脚本「未归属字节率 = 0」验收。**「把 primary console 移到 UART0」是伪需求**（现状已是 UART0）（嵌入式） | 路线已冻结，待上板验证 |
| R2 | **EDV11P 模组固件不支持 0x70 目标上报 / 斜装坐标** | 1 | 5 | **5** | — | ✅ **已关闭（2026-10-22）**：固件 `V1.3.3`，`0x70` 带坐标稳定上报；回读证明配置为侧装口径（挂高 200cm、x±500/y0-800、高 30~200cm） | **已关闭** |
| R3 | **模组插错排针（H4 而非 H3）**：H4 顺序相反，电源与数据互扰 | 3 | 4 | 12 | 无任何数据；RADAR 红灯异常 | 上板 checklist 固化为第一步；插前核对丝印 `RES A R T G V P`（硬件） | 已有教训沉淀 |
| R4 | **921600 下 USB CDC 背压/丢字节**：USB 主机端读得慢时可能溢出 | 2 | 4 | **8** | 帧校验报告 msgid 不连续、未归属字节率 > 0 | 固件改为「单批 60 ms 写预算 + UART RX 16 KiB（≈177.8 ms 缓冲，2.96× 余量）+ 丢弃计数器（`uart_rx_overflows`/`usb_tx_dropped`）」；首轮 4.4 s 抽样 msgid 分组跳号 **0** | 待 30 分钟长采集定论 |
| R5 | **网页版不可用的环境限制**：Web Serial 仅 Chromium 系、且必须 https 或 localhost；用户用 Safari/Firefox 或 http 环境打不开 | 3 | 3 | 9 | 按钮无反应、`navigator.serial undefined` | 页面显式检测并给出中文提示（含替代方案：用桌面工具）；文档站是 https，满足前提（全栈） | 待实现 |
| R6 | **文档站四坑复发**：versioned 镜像不同步、`onBrokenLinks:'throw'`、静态资源要 `pathname:///`、md 链接要带 `.md`；另有 UTF-8 BOM 与 OneDrive/构建 EPERM | 3 | 4 | 12 | 本地 build 失败；线上 404 | 发布前逐条对照 `agents.md` 第 6 节；构建用 `--out-dir` 到临时目录；改 current 必须同步版本镜像（技术文档） | 待发布阶段 |
| R7 | **文档站仓库在工作区沙箱外**，写文件需一次授权审批，可能被拒 | 3 | 3 | 9 | 写文件报 `sandbox: file access denied` | 先在工程内完成全部内容，再一次性申请授权落库；若拒绝则交付「可直接复制」的补丁清单（项目经理） | 待发布阶段 |
| R8 | **上板实测依赖用户插板**：当前本机无任何 COM 口，T2-* 全阻塞 | 4 | 3 | 12 | 扫描无 COM；任务停在待实测 | 固件与网页工具先行；把「插板 + 给 COM 号」作为明确的用户动作请求；实测未完成时 Gate-2 只能 GO-with-conditions（项目经理） | 已识别 |
| R9 | **斜装几何与实际安装不符**：倾角/挂高/离墙距离与 recipe 写的不一致 → 轨迹整体偏移 | 3 | 3 | 9 | 轨迹与人实际位置系统性偏差（> 30cm）且方向一致 | recipe 给出参数表与「画地面十字标定法」；实测用固定走位验证（硬件 + 测试） | 待标定 |
| R10 | **构建回归破坏其它 demo**：无条件 `#error` 或 Kconfig 写错导致 17 个 demo 全崩 | 2 | 4 | 8 | 全量构建失败 | 遵守「断言必须 `#if defined(CONFIG_APP_EDK60_DEMO_XXX)` 绑定」；`build_all.ps1 -Size` 全量回归（嵌入式） | 制度已固化 |
| R11 | **型号分支未定**：EDV11P-N 系列 -02 = 人数统计/轨迹跟踪、-03 = 跌倒+人存；用户口头称 -02，本地 6 个 hex 全为 -03 且无 -02 标识 | 1 | 5 | **5** | — | ✅ **已关闭（2026-10-22 上板定性）**：`GET 0x02` 回读 `EDV11P_VA,V1.3.3,0x95F0`；`0x70` 正常上报且带坐标；工作模式=1、挂高=200cm、范围 x±500/y0-800、高度 30~200cm → 人员定位侧装版成立（见 `test-report-onboard.md` 第二、三节） | **已关闭** |
| R13 | **构建脚本对 stderr 过敏**：`build_all.ps1` 设 `$ErrorActionPreference='Stop'`，IDF 6.1 的 Kconfig `NOTE`（`BT_NIMBLE_MESH_PROVISIONER`）被当作异常中断 → 「假失败」（demo 实际从未构建成功） | 4 | 2 | 8 | `-Demo xxx` 输出 `[异常] NOTE: ...`，Summary 显 FAIL 且 build 目录无产物 | 在 `Invoke-OneDemoBuild` 内用 `$ErrorActionPreference='Continue'` 包住 idf.py 调用，仅以 `$LASTEXITCODE` 判成败（嵌入式） | **已复现，待修（P2）** |
| R12 | **供电峰值能力不足（头号风险）**：EDV11P 启动峰值 **<1200 mA**、平均 <190 mA。**LDO 已核实：AP7362-33SP-13 额定 1.5 A（固定 3.3V，SO8-EP），对 1.2A 峰值留 25% 余量 → LDO 不是短板**；真正短板是 5V 输入源：PC 的 USB2=500mA / USB3=900mA 不足以覆盖 1.2A 浪涌，会把 5V 轨拉塌 → 主 3.3V 跟着塌 → MCU 复位，现象像「板子坏了」 | 4 | 3 | **12** | 上电瞬间重启循环、USB 掉枚举、RADAR 红灯闪烁 | 板上有 DC-005 座（J1）+ 理想二极管 ORing：**DC-005 接 5V/≥2A 适配器**（「USB-C 只传数据」物理上不成立，关键是双源 ORing 行为 → 待实测 T-09）；软件侧新增丢弃计数（P1-04）；仍不足则并 470µF+100nF（硬件 + 用户） | **LDO 参数已闭环（1.5A），风险降至 R=12** |

## 升级阈值

- R ≥ 15：必须在 Gate 评审中逐条给结论，未闭环不得放行。
- 任一风险变为「已发生」→ 立即升级用户并给出 2 个可选处置方案。
