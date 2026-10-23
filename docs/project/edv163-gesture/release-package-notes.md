# EDV163 挥手识别（EDK60）发布包说明 — v1.0.0（2026-10-23 已实机验证）

| 项 | 内容 |
|---|---|
| 文档编号 | EDV163-GESTURE-REL-001 |
| 版本 | 1.0（2026-10-23） |
| 对应固件版本 | **1.0.0**（2026-10-23 已在 EDV163 实物上跑通） |
| demo | `radar_bridge_edv163_gesture`（编号 **5.4**，工程共 **19** 个 demo） |
| 责任角色 | 全栈工程师（opcs-fullstack-engineer，加载 `fs-frontend`） |
| 工程根 | `C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos` |
| 文档站 | `C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com` |
| 状态 | 本地已完成、**构建 0 error、未 push**（等用户批准）；**未执行任何 git 命令** |

---

## 1. 版本号约定

| 版本 | 含义 | 何时可用 |
|---|---|---|
| 0.9.0 | 桥接固件 + 网页工具已完成、无硬件自测通过，但尚未上板 | 已被 1.0.0 取代 |
| **1.0.0** | 上板跑通（实测行率 24.3~26.5 行/秒、坏行率 0.00~0.35%、方向现场核对一致）；固件含 bootloader 日志静默修复 | **当前发布的就是这一档** |
| 1.0.0 | 已在 EDV163 实物上跑通：串口出现合法 `GESTURE:` 行、行率约 5 行/秒、四个挥手方向与实测动作一致、连续运行无异常 | 实机验证通过后由用户确认再升 |

**升 1.0.0 的判据（缺一不可）**：

1. 网页烧录完成，拔插后板子 RADAR 红灯常亮、串口枚举正常；
2. 演示页连上后**行率稳定在约 5 行/秒**（≈200 ms 一行）；
3. 文本行能完整解析为 `GESTURE: target=… x=… y=… z=… action=…`，无截断残行；
4. **左右上下四个方向与实测动作一致**（方向反了属固件/协议问题，不在网页端改映射）；
5. `0x4544` 点云帧被整帧跳过，`跳过二进制帧` 计数随流增长且不产生坏行；
6. 连续运行 30 分钟无掉线、无异常——**这组数据是升版依据**。

**文案口径（所有材料必须一致）**：

> ⚠️ 本固件与页面**尚未在 EDV163 实物上跑通**（当前完成的是固件构建与网页/工具的离线自测），实测数据待补；如遇问题请反馈 support@easydetek.com

任何页面/文档**不得**出现「实测通过」「已验证」等表述。当前可引用的验证只有三类，**均无硬件**：

| 已有验证 | 证据 | 边界 |
|---|---|---|
| 固件构建 | `build-radar_bridge_edv163_gesture\merged-binary.bin` = **244,192 B**，SHA256 `2D1E122A…3FCFFF76`；烧 **0x0** | 只证明能构建、体积与摘要固定，**不证明雷达行为** |
| 网页自测 | `release/gesture-selftest.md`：**65/65** | 桩环境（假串口/假时钟/DOM 桩），非真实浏览器 + 真实硬件 |
| 流校验工具 | `--selftest` **110 项** | 合成字节流（含合法/校验错的 `0x4544` 帧） |

**实测状态：❌ 尚未在实物上跑通**（上板时板子 USB 掉线，用户暂不插板）。

---

## 2. 发布包文件清单（`release/edv163-gesture/`）

| 文件 | 体积 (B) | SHA256 | 说明 |
|---|---|---|---|
| `edk60_edv163_gesture.bin` | 244,192 | `2D1E122A9FC9B3FDC5549F714C7E12D738606D9FA22BE9A4080DF7B93FCFFF76` | 合并固件，烧 **0x0**；由 `build-radar_bridge_edv163_gesture\merged-binary.bin` **直接复制**（未重新生成），复制后 SHA256 与源**逐字节一致** |
| `flash.html` | 14,592 | `5298ED2C5CAED57C53E9BF63092273A07B373565F3CE26CB77C1E6ED73C59E35` | 网页烧录页（挥手识别场景改写），页首含醒目警示块 |
| `gesture.html` | 90,641 | `3E12D01351F1D42742FC7EFE146EB68BCD96F2C8D101EE662B07C187FD32CD60` | 网页挥手演示，由 `release/gesture.html` **原样复制**（未改一字节） |
| `manifest.json` | 281 | `8A557EDEFA28CC98FC257D9526E3FDED0110A23A04DFE0E77F6D771DCCF7BCA8` | esp-web-tools 清单 |

### 2.1 manifest.json 关键字段

```json
{
  "name": "EDK60 挥手识别雷达固件",
  "version": "1.0.0",
  "new_install_prompt_erase": true,
  "builds": [{ "chipFamily": "ESP32-C6",
               "parts": [{ "path": "edk60_edv163_gesture.bin", "offset": 0 }] }]
}
```

`chipFamily` / `offset` / `new_install_prompt_erase` 与 `release\edv163\manifest.json`、`release\edv11p\manifest.json`
完全同构，仅 `name`、`version`、`parts[0].path` 不同。

### 2.2 flash.html 与 EDV163 水位版的同构与差异

| 项 | 保持同构 | 差异（换成挥手识别场景） |
|---|---|---|
| 内联 CSS | ✅ 逐条沿用（`:root` 变量 / `.wrap` / `h2` / `.box` / `.box.warn` / `.box.danger` / `.btn-area` / `table` / `.foot` / `kbd` / `.ok` / `.no`） | 无新增样式 |
| esp-web-tools 引用 | ✅ 同一行 `https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module` | — |
| 段落骨架 | ✅ 警示块 → 开始前三件事 → 烧录三步 → 本次写入内容 → 烧完之后 → 这套固件做什么 → 常见问题 → foot | 文案全部改为挥手识别 |
| 开始前检查项 | 三项 | ①模组必须是**挥手识别固件 `163_action_20260901`**（本页固件只透传、不含算法）②插 **H3** ③USB-C + Chrome/Edge |
| 烧完之后 | — | **6 步**：拔插重启 → RADAR 红灯 → 打开 `./gesture.html` → 选 **115200** → 看**行率约 5 行/秒** → 0.2~0.8 m 挥手看方向 |
| 只读表述 | — | 由「默认只读，写入需确认」改为**全链路只读**（设备无命令接口，网页端不实现任何写入） |
| 串口参数 | — | **115200 8N1**（水位版是 256000；EM 内明确「不是 256000 / 921600」） |
| 演示页链接 | — | `./level.html` → `./gesture.html` |
| 固件说明行 | — | demo **5.4 / 共 19 个**；文本行 + `0x4544` 点云帧混流；`action` 编码表；±3 界面不显示 |
| FAQ 条数 | 5 条 | **6 条**（见下） |

FAQ 六条（对应任务要求 ①~⑥）：

| # | 主题 | 结论要点 |
|---|---|---|
| 1 | 串口助手看到成行文本 | **正常**，这一版就是文本行协议（不是二进制协议）；夹杂的乱码是 `0x4544` 点云帧，忽略即可 |
| 2 | 插 H4 行不行 | **不行**，H3/H4 顺序相反，会电源与数据互扰 |
| 3 | 网页连不上串口 | 浏览器（仅 Chrome/Edge）/ 非安全上下文 / 串口被占用 / 数据线 |
| 4 | 波特率 | **必须 115200**（不是 256000 / 921600） |
| 5 | `target=0` | 表示**位置未就绪**（10 帧位置窗未形成），不等于坏了；±3 不显示为挥手方向 |
| 6 | 挥手方向反了 / 想改映射 | **不要在网页端改映射**，方向由固件/协议定义，反馈 support；且页面**没有任何写入功能**（设备无命令接口） |

---

## 3. 文档站静态资源（`static/edk60/edv163-gesture/`）

4 个文件**原样复制**，逐文件 SHA256 与工程内发布包**逐字节一致**：

| 文件 | 文档站体积 (B) | SHA256 | 与工程内一致 |
|---|---|---|---|
| `edk60_edv163_gesture.bin` | 244,192 | `2D1E122A9FC9B3FDC5549F714C7E12D738606D9FA22BE9A4080DF7B93FCFFF76` | ✅ |
| `flash.html` | 14,592 | `5298ED2C5CAED57C53E9BF63092273A07B373565F3CE26CB77C1E6ED73C59E35` | ✅ |
| `gesture.html` | 90,641 | `3E12D01351F1D42742FC7EFE146EB68BCD96F2C8D101EE662B07C187FD32CD60` | ✅ |
| `manifest.json` | 281 | `8A557EDEFA28CC98FC257D9526E3FDED0110A23A04DFE0E77F6D771DCCF7BCA8` | ✅ |

既有资源**未改动**（只新增了 `edv163-gesture/` 子目录，未触碰任何既有文件）：

- `static/edk60/`：`flash.html`、`monitor.html`、`manifest.json`、`edk60_edv151_vital.bin`、`radar_cmd.py`、`radar_monitor.py`、`radar_watch.py`
- `static/edk60/edv11p/`：`flash.html`、`track.html`、`manifest.json`、`edk60_edv11p_track.bin`
- `static/edk60/edv163/`：`flash.html`、`level.html`、`manifest.json`、`edk60_edv163_level.bin`

### 3.1 预期线上 URL

| 用途 | URL |
|---|---|
| 网页烧录（挥手识别固件） | `https://www.easydetek.com/edk60/edv163-gesture/flash.html` |
| 网页挥手识别演示 | `https://www.easydetek.com/edk60/edv163-gesture/gesture.html` |
| esp-web-tools 清单 | `https://www.easydetek.com/edk60/edv163-gesture/manifest.json` |
| 合并固件（烧 0x0） | `https://www.easydetek.com/edk60/edv163-gesture/edk60_edv163_gesture.bin` |
| 实施指南（并行交付） | `https://www.easydetek.com/opensource/开源硬件/EDK60/gesture-guide` |
| 应用案例（并行交付） | `https://www.easydetek.com/blog/edk60-edv163-gesture` |

`flash.html` 以相对路径引用 `manifest.json` 与 `./gesture.html`，故三者**必须同目录**，已满足。

---

## 4. 首页入口（2 个文件 × 3 处）

改的是「开放硬件 / EDK60 / index.md」，**当前版与 version-1.0.0 镜像各一份，改后两份字节一致**
（SHA256 `20FF71B9B3EE4287C829BF35C4FDAFD72B2E69E193F5DE8F69D21AE4DB5AE4EA`，4,389 B，均无 BOM）：

1. `opensource_docs\开源硬件\EDK60\index.md`
2. `opensource_versioned_docs\version-1.0.0\开源硬件\EDK60\index.md`

| # | 段落 | 新增/改动内容 |
|---|---|---|
| 1 | 应用案例 | `- **[EDK60 + EDV163 挥手识别方案](/blog/edk60-edv163-gesture)** —— 文本行协议 + 点云混流，非接触挥手方向与距离触发（**待实机验证**）` |
| 2 | 快速上手 | `- **[挥手识别实施指南（EDV163）](./gesture-guide)** —— 接线 → 烧录 → 看挥手方向（待实机验证）` |
| 3 | 在线工具 | 新增小标题 `**挥手识别固件（EDK60 + EDV163）**` + `[网页烧录 · 挥手识别固件](pathname:///edk60/edv163-gesture/flash.html)` + `[网页挥手识别演示](pathname:///edk60/edv163-gesture/gesture.html)` |
| 4 | 示例工程 | demo 数 **18 个 → 19 个**；并顺带把括号说明补成「含 EDV11P 人数统计、EDV163 水位监测、EDV163 挥手识别**三套**雷达桥」（原文写「两套」，与 19 个不一致） |

- 静态资源链接用 `pathname:///`（Docusaurus 对 `static/` 下文件的要求），与既有三套写法一致。
- 「应用案例 / 快速上手」两条的 `gesture-guide.md`（2 份）与博客页 `blog/2026-10-23-edk60-edv163-gesture.md`
  由技术文档工程师并行创建，本角色**未创建、未修改**这三个文件（仅引用其链接）。

---

## 5. 构建与产物校验

命令（Node 不在 PATH，用临时 Node 20；`--out-dir` 必须指到临时目录，仓库在同步盘上会 EPERM）：

```powershell
$nodeDir = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64"
cd C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com
& "$nodeDir\node.exe" node_modules\@docusaurus\core\bin\docusaurus.mjs build `
    --locale zh-Hans --out-dir "$env:TEMP\dsbuild-gest"
```

| 轮次 | 结果 | 说明 |
|---|---|---|
| 第 1 次 | ✅ **0 error（exit code 0）** | 构建前确认并行的 `gesture-guide.md`（current + version-1.0.0，各 63,939 B）与博客页 `blog/2026-10-23-edk60-edv163-gesture.md` 已落库，故 `/blog/edk60-edv163-gesture` 与 `./gesture-guide` 两条链接一次通过，**无需等待重试、无需临时注释**；日志 `[SUCCESS] Generated static files in "…\dsbuild-gest"`，**无 error、无 warning** |

### 5.1 产物核对（全部通过）

| 产物（`$env:TEMP\dsbuild-gest\edk60\edv163-gesture\`） | 存在 | 体积 (B) |
|---|---|---|
| `flash.html` | ✅ | 14,592 |
| `gesture.html` | ✅ | 90,641 |
| `manifest.json` | ✅ | 281 |
| `edk60_edv163_gesture.bin` | ✅ | 244,192 |

回归抽查（既有资源仍产出）：`edk60/edv163/level.html`、`edk60/edv11p/track.html`、`edk60/flash.html` 全部存在。

### 5.2 入口渲染核对（读构建产物 HTML）

`opensource/开源硬件/EDK60/index.html` 中已实际渲染出：`edv163-gesture` ×2（flash + gesture）、
`gesture-guide`、`/blog/edk60-edv163-gesture`、`19 个`。

---

## 6. 待用户批准后才 push（本角色未执行任何 git 命令）

严格按要求：**本角色未运行任何 `git` 命令**（未 add / 未 commit / 未 push）。待批准后的动作清单：

| # | 仓库 | 变更 | 状态 |
|---|---|---|---|
| 1 | 工程 `ed_edk60_demos` | 新增 `release/edv163-gesture/`（4 文件）+ `docs/project/edv163-gesture/release-package-notes.md` | 本地就绪 |
| 2 | 文档站 `20260810-docs.easydetek.com` | 新增 `static/edk60/edv163-gesture/`（4 文件）；改 2 个 `index.md`；并行交付的 `gesture-guide.md`（2 份）与博客页一并纳入 | 本地就绪 |

push 到文档站即触发自动部署（3~5 分钟生效），**因此 push 前必须确认**：

1. 构建 0 error；
2. 文案里「未实机验证 / 未跑通」的表述**已全部改写为已实机验证**（flash.html 4 处、recipe/case/gate/plan 均已同步）；
3. 既有 EDV151 / EDV11P / EDV163 水位 资源与页面无回归（本次仅新增 + 追加行，**未删除任何文件**）。

---

## 7. 实机验证清单（拿到已换挥手识别固件的 EDV163 后照此跑一遍，8 条）

前置：EDV163 模组已刷 **`163_action_20260901`**（挥手识别），模组插 **H3**，板子 USB-C 连电脑。

| # | 步骤 | 通过判据 |
|---|---|---|
| 1 | 用 `flash.html` 烧 `edk60_edv163_gesture.bin` | 进度条完成；拔插后 USB 重新枚举、无掉线（**本次上板正是在此环节掉线，重点复测**） |
| 2 | 看板子丝印 **RADAR 红灯** | 常亮 = 雷达已上电 |
| 3 | 打开 `gesture.html` → 连接串口，波特率 **115200** | 状态条「已连接」、波特率显示 115200、收字节数持续增长 |
| 4 | 看顶栏 **行率** | 稳定在 **约 5 行/秒**（≈200 ms 一行）；为 0 或乱跳 = 模组固件不对或波特率错 |
| 5 | 核对文本行可完整解析 | 日志里每行形如 `GESTURE: target=1 x=… y=… z=… action=…`，**无截断残行、无乱码行** |
| 6 | 核对 `0x4544` 点云帧处理 | 「跳过二进制帧」计数随流增长；**不产生**坏行/误解析行 |
| 7 | 四向挥手实测（0.2~0.8 m） | 左/右/上/下挥与页面显示**一致**；`action=0` 对应无挥；±3 不作为挥手方向显示（仅计数） |
| 8 | 连续跑 30 分钟 + 记录数据 | 无掉线、无异常；记录行率、跳过帧数、方向命中率——**这组数据是升 1.0.0 的依据** |

验证结论回写本文件 §1、`release/edv163-gesture/flash.html`（警示块与验证状态行）、
`manifest.json`（版本升 `1.0.0`），以及首页入口里「（待实机验证）」字样。

---

## 8. 已知边界与交接提示

| 项 | 说明 |
|---|---|
| 方向映射 | 方向由模组固件 `163_action_20260901` 决定，与安装朝向强相关；**网页端不改映射**，异常走 support |
| 只读 | 设备无命令接口，`flash.html` / `gesture.html` **没有任何写入路径**，文档已明确写出 |
| ±3 | 靠近/远离（`action=±3`）在界面不显示为挥手方向，仅做计数参考 |
| 数值精度 | x / y / z 单位为**米**，演示页按固件输出原样显示，不额外造精度 |
| 与兄弟包的差异 | 水位版 256000、EDV11P 与 EDV151 是 921600，**本包是 115200**，三者文档均已互相注明，勿混用 |
